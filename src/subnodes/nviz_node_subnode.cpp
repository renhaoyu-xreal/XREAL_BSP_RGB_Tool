/*
 * NvizNodeSubnode 实现
 *
 * 包含完整的 UDP 和 TCP 数据接收，以及二进制负载解析。
 */
#include "recordlab/subnodes/nviz_node_subnode.h"

#include "recordlab/common/topics.h"
#include "recordlab/core/legacy_config_loader.h"
#include "recordlab/core/usb_device_catalog.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <chrono>
#include <cstring>
#include <csignal>
#include <iostream>
#include <QElapsedTimer>
#include <QThread>

namespace recordlab::subnodes {

using json = nlohmann::json;

static void logToFile(const std::string &) {}

namespace {

struct ImuSourceProbe {
  uint64_t count = 0;
  int64_t lastArrivalNs = 0;
  int64_t lastTimestampNs = 0;
  int64_t maxArrivalGapNs = 0;
  int64_t maxTimestampGapNs = 0;
  int64_t maxTimestampBacktrackNs = 0;
};

int64_t steadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void logImuSourceProbe(const char *tag, ImuSourceProbe &probe,
                       int64_t arrivalNs, int64_t timestampNs, int imuType,
                       int groupId, int msgId) {
  ++probe.count;
  int64_t arrivalGapNs = 0;
  int64_t timestampGapNs = 0;
  if (probe.lastArrivalNs > 0) {
    arrivalGapNs = arrivalNs - probe.lastArrivalNs;
    if (arrivalGapNs > probe.maxArrivalGapNs) {
      probe.maxArrivalGapNs = arrivalGapNs;
    }
  }
  if (probe.lastTimestampNs > 0) {
    timestampGapNs = timestampNs - probe.lastTimestampNs;
    if (timestampGapNs > probe.maxTimestampGapNs) {
      probe.maxTimestampGapNs = timestampGapNs;
    }
    if (timestampGapNs < 0) {
      const int64_t backtrackNs = -timestampGapNs;
      if (backtrackNs > probe.maxTimestampBacktrackNs) {
        probe.maxTimestampBacktrackNs = backtrackNs;
      }
    }
  }
  probe.lastArrivalNs = arrivalNs;
  probe.lastTimestampNs = timestampNs;

  if (probe.count <= 20 || probe.count % 1000 == 0) {
    std::cout << "[" << tag << "] count=" << probe.count
              << " group=" << groupId << " msg=" << msgId
              << " imu_type=" << imuType
              << " arrival_gap_ms=" << static_cast<double>(arrivalGapNs) / 1e6
              << " timestamp_gap_ms="
              << static_cast<double>(timestampGapNs) / 1e6
              << " max_arrival_gap_ms="
              << static_cast<double>(probe.maxArrivalGapNs) / 1e6
              << " max_timestamp_gap_ms="
              << static_cast<double>(probe.maxTimestampGapNs) / 1e6
              << " max_timestamp_backtrack_ms="
              << static_cast<double>(probe.maxTimestampBacktrackNs) / 1e6
              << std::endl;
  }
}

std::vector<double> parseStandardNrealPayload(const QByteArray &payload) {
  std::vector<double> result;
  constexpr int kMinPayloadSize = 8 + 8 + 8 + 4 + 6 * 4;
  if (payload.size() < kMinPayloadSize) {
    return result;
  }

  const auto *raw = reinterpret_cast<const uint8_t *>(payload.constData());
  int offset = 0;
  auto readU64 = [&]() {
    uint64_t v = 0;
    std::memcpy(&v, raw + offset, sizeof(v));
    offset += static_cast<int>(sizeof(v));
    return static_cast<double>(v);
  };
  auto readU32 = [&]() {
    uint32_t v = 0;
    std::memcpy(&v, raw + offset, sizeof(v));
    offset += static_cast<int>(sizeof(v));
    return static_cast<double>(v);
  };
  auto readF32 = [&]() {
    float v = 0.0f;
    std::memcpy(&v, raw + offset, sizeof(v));
    offset += static_cast<int>(sizeof(v));
    return static_cast<double>(v);
  };

  result.push_back(readU64()); // ts_us
  result.push_back(readU64()); // onsensor_timestamp_us
  result.push_back(readU64()); // timestamp_ns
  result.push_back(readU32()); // type
  for (int i = 0; i < 6; ++i) {
    result.push_back(readF32());
  }
  return result;
}

} // namespace

// ============================================================================
// PayloadParser 实现
// ============================================================================

PayloadParser::PayloadParser(const QString &plotJsonPath) {
  QFile file(plotJsonPath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    std::cerr << "[PayloadParser] Failed to open: "
              << plotJsonPath.toStdString() << std::endl;
    return;
  }

  try {
    auto config = json::parse(file.readAll().toStdString());
    for (auto it = config.begin(); it != config.end(); ++it) {
      const auto &msgConfig = it.value();
      if (!msgConfig.contains("GROUP_ID") || !msgConfig.contains("MSG_ID")) {
        continue;
      }

      int groupId = msgConfig["GROUP_ID"].get<int>();
      int msgId = msgConfig["MSG_ID"].get<int>();

      PayloadParser::MessageTemplate templ;
      templ.messageName = it.key();
      if (msgConfig.contains("struct") && msgConfig["struct"].is_array()) {
        for (const auto &fieldDef : msgConfig["struct"]) {
          const std::string fieldStr = fieldDef.get<std::string>();
          auto spacePos = fieldStr.find(' ');
          if (spacePos == std::string::npos) {
            continue;
          }

          std::string typeStr = fieldStr.substr(0, spacePos);
          std::string namePart = fieldStr.substr(spacePos + 1);
          const bool hidden = namePart.find("HIDE") != std::string::npos;

          int arraySize = 1;
          auto bracketPos = namePart.find('[');
          if (bracketPos != std::string::npos) {
            auto endBracket = namePart.find(']', bracketPos);
            if (endBracket != std::string::npos) {
              auto sizeStr =
                  namePart.substr(bracketPos + 1, endBracket - bracketPos - 1);
              try {
                arraySize = std::stoi(sizeStr);
              } catch (...) {
                arraySize = 1;
              }
            }
          }
          std::string baseName = namePart;
          if (bracketPos != std::string::npos) {
            baseName = namePart.substr(0, bracketPos);
          }
          if (baseName.find("ts_") != std::string::npos) {
            baseName = "timestamp";
          }

          auto spec = parseTypeString(typeStr, arraySize);
          spec.name = baseName;
          spec.type = typeStr;
          spec.hidden = hidden;
          templ.fields.push_back(std::move(spec));
        }
      }

      templates_[makeKey(groupId, msgId)] = std::move(templ);
    }

    loaded_ = true;
    std::cout << "[PayloadParser] Loaded " << templates_.size()
              << " message templates from " << plotJsonPath.toStdString()
              << std::endl;
    logToFile("[PayloadParser] Loaded templates=" +
              std::to_string(templates_.size()) + " path=" +
              plotJsonPath.toStdString());
  } catch (const std::exception &e) {
    std::cerr << "[PayloadParser] Parse error: " << e.what() << std::endl;
  }
}

bool PayloadParser::hasTemplate(int groupId, int msgId) const {
  return templates_.find(makeKey(groupId, msgId)) != templates_.end();
}

std::string PayloadParser::messageName(int groupId, int msgId) const {
  auto it = templates_.find(makeKey(groupId, msgId));
  return it != templates_.end() ? it->second.messageName : std::string();
}

PayloadParser::FieldSpec PayloadParser::parseTypeString(const std::string &typeStr,
                                                        int arraySize) {
  FieldSpec spec{};
  spec.count = arraySize;
  spec.isSigned = false;
  spec.isFloat = false;
  spec.hidden = false;

  if (typeStr == "u8") {
    spec.byteSize = 1;
  } else if (typeStr == "s8") {
    spec.byteSize = 1;
    spec.isSigned = true;
  } else if (typeStr == "u16") {
    spec.byteSize = 2;
  } else if (typeStr == "s16") {
    spec.byteSize = 2;
    spec.isSigned = true;
  } else if (typeStr == "u32") {
    spec.byteSize = 4;
  } else if (typeStr == "s32") {
    spec.byteSize = 4;
    spec.isSigned = true;
  } else if (typeStr == "u64") {
    spec.byteSize = 8;
  } else if (typeStr == "s64") {
    spec.byteSize = 8;
    spec.isSigned = true;
  } else if (typeStr == "f32") {
    spec.byteSize = 4;
    spec.isFloat = true;
  } else if (typeStr == "f64") {
    spec.byteSize = 8;
    spec.isFloat = true;
  } else {
    spec.byteSize = 4;
  }

  return spec;
}

std::vector<double> PayloadParser::parse(int groupId, int msgId,
                                         const QByteArray &payload) const {
  auto key = makeKey(groupId, msgId);
  auto it = templates_.find(key);
  if (it == templates_.end()) {
    std::vector<double> result;
    int offset = 0;
    const int len = payload.size();
    const auto *raw = reinterpret_cast<const uint8_t *>(payload.constData());
    while (offset + 8 <= len) {
      uint64_t v;
      std::memcpy(&v, raw + offset, 8);
      result.push_back(static_cast<double>(v));
      offset += 8;
    }
    while (offset + 4 <= len) {
      float v;
      std::memcpy(&v, raw + offset, 4);
      result.push_back(static_cast<double>(v));
      offset += 4;
    }
    return result;
  }

  const auto &templ = it->second.fields;
  std::vector<double> result;
  int offset = 0;
  const int len = payload.size();
  const auto *raw = reinterpret_cast<const uint8_t *>(payload.constData());

  for (const auto &field : templ) {
    for (int i = 0; i < field.count; ++i) {
      if (offset + field.byteSize > len) {
        return result;
      }

      double value = 0.0;
      if (field.isFloat) {
        if (field.byteSize == 4) {
          float v;
          std::memcpy(&v, raw + offset, 4);
          value = static_cast<double>(v);
        } else if (field.byteSize == 8) {
          double v;
          std::memcpy(&v, raw + offset, 8);
          value = v;
        }
      } else if (field.isSigned) {
        switch (field.byteSize) {
        case 1: { int8_t v; std::memcpy(&v, raw + offset, 1); value = static_cast<double>(v); break; }
        case 2: { int16_t v; std::memcpy(&v, raw + offset, 2); value = static_cast<double>(v); break; }
        case 4: { int32_t v; std::memcpy(&v, raw + offset, 4); value = static_cast<double>(v); break; }
        case 8: { int64_t v; std::memcpy(&v, raw + offset, 8); value = static_cast<double>(v); break; }
        }
      } else {
        switch (field.byteSize) {
        case 1: { uint8_t v; std::memcpy(&v, raw + offset, 1); value = static_cast<double>(v); break; }
        case 2: { uint16_t v; std::memcpy(&v, raw + offset, 2); value = static_cast<double>(v); break; }
        case 4: { uint32_t v; std::memcpy(&v, raw + offset, 4); value = static_cast<double>(v); break; }
        case 8: { uint64_t v; std::memcpy(&v, raw + offset, 8); value = static_cast<double>(v); break; }
        }
      }

      result.push_back(value);
      offset += field.byteSize;
    }
  }

  return result;
}

std::vector<PayloadParser::ParsedField>
PayloadParser::parseFields(int groupId, int msgId,
                           const QByteArray &payload) const {
  auto it = templates_.find(makeKey(groupId, msgId));
  if (it == templates_.end()) {
    return {};
  }

  std::vector<ParsedField> result;
  int offset = 0;
  const int len = payload.size();
  const auto *raw = reinterpret_cast<const uint8_t *>(payload.constData());

  for (const auto &field : it->second.fields) {
    for (int i = 0; i < field.count; ++i) {
      if (offset + field.byteSize > len) {
        return result;
      }

      double value = 0.0;
      if (field.isFloat) {
        if (field.byteSize == 4) {
          float v;
          std::memcpy(&v, raw + offset, 4);
          value = static_cast<double>(v);
        } else if (field.byteSize == 8) {
          double v;
          std::memcpy(&v, raw + offset, 8);
          value = v;
        }
      } else if (field.isSigned) {
        switch (field.byteSize) {
        case 1: { int8_t v; std::memcpy(&v, raw + offset, 1); value = static_cast<double>(v); break; }
        case 2: { int16_t v; std::memcpy(&v, raw + offset, 2); value = static_cast<double>(v); break; }
        case 4: { int32_t v; std::memcpy(&v, raw + offset, 4); value = static_cast<double>(v); break; }
        case 8: { int64_t v; std::memcpy(&v, raw + offset, 8); value = static_cast<double>(v); break; }
        }
      } else {
        switch (field.byteSize) {
        case 1: { uint8_t v; std::memcpy(&v, raw + offset, 1); value = static_cast<double>(v); break; }
        case 2: { uint16_t v; std::memcpy(&v, raw + offset, 2); value = static_cast<double>(v); break; }
        case 4: { uint32_t v; std::memcpy(&v, raw + offset, 4); value = static_cast<double>(v); break; }
        case 8: { uint64_t v; std::memcpy(&v, raw + offset, 8); value = static_cast<double>(v); break; }
        }
      }

      if (!field.hidden) {
        std::string name = field.name;
        if (field.count > 1) {
          name += "[" + std::to_string(i) + "]";
        }
        result.push_back(ParsedField{name, field.type, value});
      }
      offset += field.byteSize;
    }
  }

  return result;
}

// ============================================================================
// NrealLinkTcpClient 实现
// ============================================================================

uint8_t NrealLinkTcpClient::crc8(const uint8_t *data, int size) {
  uint8_t crc = 0x00;
  uint8_t poly = 0x07;
  for (int i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; ++j) {
      if (crc & 0x80) {
        crc = (uint8_t)((crc << 1) ^ poly);
      } else {
        crc = (uint8_t)(crc << 1);
      }
    }
  }
  return crc;
}

NrealLinkTcpClient::NrealLinkTcpClient(QTcpSocket *socket, DataCallback cb,
                                       QObject *parent)
    : QObject(parent), socket_(socket), callback_(std::move(cb)) {
  connect(socket_, &QTcpSocket::readyRead, this, &NrealLinkTcpClient::onReadyRead);
  connect(socket_, &QTcpSocket::disconnected, this, &NrealLinkTcpClient::onDisconnected);
}

NrealLinkTcpClient::~NrealLinkTcpClient() {
  if (socket_) {
    socket_->disconnectFromHost();
    socket_->deleteLater();
  }
}

void NrealLinkTcpClient::onDisconnected() {
  std::cout << "[NrealLinkTcpClient] Client disconnected" << std::endl;
  deleteLater();
}

void NrealLinkTcpClient::onReadyRead() {
  if (handlingReadyRead_) {
    readyReadAgain_ = true;
    return;
  }
  handlingReadyRead_ = true;

  static uint64_t readyReadCount = 0;
  do {
    readyReadAgain_ = false;
  while (socket_->bytesAvailable() > 0) {
    QByteArray data = socket_->readAll();
    if (data.isEmpty()) {
      continue;
    }

    // The sender prefixes every TCP write with TcpPacketMsgHeader. This mirrors
    // the Python implementation, which strips one byte per recv() call.
    const uint8_t packetHeaderByte = static_cast<uint8_t>(data.at(0));
    const int submsgType = packetHeaderByte & 0x01;
    readyReadCount++;
    if (readyReadCount <= 80 || readyReadCount % 1000 == 0) {
      logToFile("[NrealLinkTcpClient][READY_READ] count=" +
                std::to_string(readyReadCount) +
                " raw_size=" + std::to_string(data.size()) +
                " submsg=" + std::to_string(submsgType) +
                " status=" + std::to_string(status_) +
                " buffer_before=" + std::to_string(buffer_.size()) +
                " bytesAvailable_after_read=" +
                std::to_string(socket_->bytesAvailable()));
    }
    data.remove(0, 1);

    if (status_ == INIT || status_ == BODY_PARSED) {
      if (submsgType != 0) {
        logToFile("[NrealLinkTcpClient] Expected HEADER packet, got DATA; dropping chunk");
        continue;
      }
    } else if (status_ == HEAD_PARSED) {
      if (submsgType == 0) {
        logToFile("[NrealLinkTcpClient] Expected DATA packet, got HEADER; resync");
        buffer_.clear();
        status_ = BODY_PARSED;
      }
    }

    buffer_.append(data);

    while (!buffer_.isEmpty()) {
      if (status_ == INIT || status_ == BODY_PARSED) {
        if (!parseHead()) {
          break;
        }
      }

      if (status_ == HEAD_PARSED) {
        if (!parseBody()) {
          break;
        }
      }
    }
  }
  } while (readyReadAgain_ && socket_ && socket_->bytesAvailable() > 0);

  handlingReadyRead_ = false;
}

bool NrealLinkTcpClient::parseHead() {
  static uint64_t waitHeadCount = 0;
  if (buffer_.size() < (int)sizeof(TcpMsgHeader)) {
    waitHeadCount++;
    if (waitHeadCount <= 20 || waitHeadCount % 1000 == 0) {
      logToFile("[NrealLinkTcpClient][WAIT_HEAD] count=" +
                std::to_string(waitHeadCount) +
                " buffer=" + std::to_string(buffer_.size()));
    }
    return false; // wait for more data
  }

  std::memcpy(&curHeader_, buffer_.constData(), sizeof(TcpMsgHeader));

  if (curHeader_.magic_num != TCP_MAGIC_NUM) {
    std::cerr << "[NrealLinkTcpClient] Invalid magic num: "
              << (int)curHeader_.magic_num << std::endl;
    logToFile("[NrealLinkTcpClient] Invalid magic num: " +
              std::to_string(curHeader_.magic_num));
    buffer_.clear();
    return false;
  }

  // Calculate CRC (set CRC byte to 0 for calculation)
  TcpMsgHeader headerForCrc = curHeader_;
  headerForCrc.crc = 0;
  uint8_t computedCrc =
      crc8(reinterpret_cast<const uint8_t *>(&headerForCrc), sizeof(TcpMsgHeader));

  if (curHeader_.crc != computedCrc) {
    std::cerr << "[NrealLinkTcpClient] CRC check failed. received="
              << (int)curHeader_.crc << ", computed=" << (int)computedCrc
              << std::endl;
    logToFile("[NrealLinkTcpClient] CRC check failed. received=" +
              std::to_string(curHeader_.crc) +
              ", computed=" + std::to_string(computedCrc));
    buffer_.clear();
    return false;
  }

  if (curHeader_.msg_count > TCP_MAX_FREQ_COUNT) {
    logToFile("[NrealLinkTcpClient] msg_count too large: " +
              std::to_string(curHeader_.msg_count));
    buffer_.clear();
    return false;
  }

  buffer_.remove(0, sizeof(TcpMsgHeader));
  status_ = HEAD_PARSED;
  logToFile("[NrealLinkTcpClient] Header parsed. msg_count=" +
            std::to_string(curHeader_.msg_count) +
            ", length=" + std::to_string(curHeader_.length));

  if (curHeader_.timestamp_ns == lastSuccessTimestamp_) {
    buffer_.clear();
    responseToClient();
    status_ = BODY_PARSED; // drop duplicate
    return false;
  }

  responseToClient();
  return true;
}

bool NrealLinkTcpClient::parseBody() {
  static uint64_t waitBodyCount = 0;
  int bodySize = curHeader_.length - sizeof(TcpMsgHeader);
  if (bodySize <= 0) {
    logToFile("[NrealLinkTcpClient] Invalid body size: " +
              std::to_string(bodySize));
    status_ = BODY_PARSED;
    return false;
  }
  if (buffer_.size() < bodySize) {
    waitBodyCount++;
    if (waitBodyCount <= 80 || waitBodyCount % 1000 == 0) {
      logToFile("[NrealLinkTcpClient][WAIT_BODY] count=" +
                std::to_string(waitBodyCount) +
                " have=" + std::to_string(buffer_.size()) +
                " need=" + std::to_string(bodySize) +
                " msg_count=" + std::to_string(curHeader_.msg_count));
    }
    responseToClient();
    return false; // wait for more data
  }

  int curIdx = 0;
  std::vector<QByteArray> allSensorDatas;

  while (curIdx < bodySize) {
    if (curIdx + (int)sizeof(NrealLinkMsgHeader) > buffer_.size())
      break;

    NrealLinkMsgHeader nrealHeader;
    std::memcpy(&nrealHeader, buffer_.constData() + curIdx,
                sizeof(NrealLinkMsgHeader));

    if (nrealHeader.payload_length < 0 ||
        nrealHeader.payload_length > bodySize ||
        nrealHeader.payload_length > buffer_.size()) {
      logToFile("[NrealLinkTcpClient] Invalid payload_length: " +
                std::to_string(nrealHeader.payload_length) +
                ", bodySize=" + std::to_string(bodySize) +
                ", buffer=" + std::to_string(buffer_.size()));
      return false;
    }
    int perSensorDataSize = sizeof(NrealLinkMsgHeader) + nrealHeader.payload_length;
    if (perSensorDataSize <= (int)sizeof(NrealLinkMsgHeader)) {
      logToFile("[NrealLinkTcpClient] Invalid sensor packet size: " +
                std::to_string(perSensorDataSize));
      return false;
    }
    if (curIdx + perSensorDataSize > buffer_.size())
      break;

    allSensorDatas.push_back(buffer_.mid(curIdx, perSensorDataSize));
    curIdx += perSensorDataSize;
  }

  if (allSensorDatas.size() != curHeader_.msg_count) {
    std::cerr << "[NrealLinkTcpClient] Bad freq_count. Parsed="
              << allSensorDatas.size()
              << ", HeaderCount=" << curHeader_.msg_count << std::endl;
    logToFile("[NrealLinkTcpClient] Bad freq_count. parsed=" +
              std::to_string(allSensorDatas.size()) +
              ", header=" + std::to_string(curHeader_.msg_count) +
              ", bodySize=" + std::to_string(bodySize));
    buffer_.clear();
    responseToClient();
    status_ = BODY_PARSED;
    return false;
  }

  buffer_.remove(0, bodySize);
  status_ = BODY_PARSED;
  lastSuccessTimestamp_ = curHeader_.timestamp_ns;

  responseToClient();
  logToFile("[NrealLinkTcpClient] Body parsed. sensor_count=" +
            std::to_string(allSensorDatas.size()) +
            " bodySize=" + std::to_string(bodySize));

  for (const auto &sensorData : allSensorDatas) {
    if (callback_) {
      callback_(sensorData);
    }
  }

  return true;
}

void NrealLinkTcpClient::responseToClient() {
  if (socket_ && socket_->isWritable()) {
    socket_->write("ok");
    socket_->flush();
  }
}

// ============================================================================
// XRLinkDevice 实现
// ============================================================================

XRLinkDevice::XRLinkDevice(const std::string &host, int udpPort, int tcpPort,
                           bool enableUdp, bool enableTcp)
    : host_(host), udpPort_(udpPort), tcpPort_(tcpPort), enableUdp_(enableUdp),
      enableTcp_(enableTcp) {
  
  logToFile("[XRLinkDevice] Constructor called. UDP=" + std::to_string(enableUdp_) + 
            ", TCP=" + std::to_string(enableTcp_));

  // 1. Load plot.json
  QString plotJsonPath;
#ifdef RECORDLABC_SOURCE_DIR
  plotJsonPath = QDir(QString::fromUtf8(RECORDLABC_SOURCE_DIR))
                     .filePath(QStringLiteral("subnodes/nviz_node/plot.json"));
#else
  plotJsonPath = QDir(QCoreApplication::applicationDirPath())
                     .filePath(QStringLiteral("subnodes/nviz_node/plot.json"));
#endif
  if (QFile::exists(plotJsonPath)) {
    payloadParser_ = PayloadParser(plotJsonPath);
  }

  // 2. Setup UDP Server
  if (enableUdp_) {
    udpSocket_ = std::make_unique<QUdpSocket>();
    udpSocket_->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption,
                                256 * 1024);
    udpSocket_->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,
                                512 * 1024);
    if (udpSocket_->bind(QHostAddress::AnyIPv4, static_cast<quint16>(udpPort_),
                         QUdpSocket::ShareAddress |
                             QUdpSocket::ReuseAddressHint)) {
      QObject::connect(udpSocket_.get(), &QUdpSocket::readyRead,
                       [this]() { onUdpData(); });
      std::cout << "[XRLinkDevice] UDP server bound to port " << udpPort_ << std::endl;
      logToFile("[XRLinkDevice] UDP server bound to IPv4 port " + std::to_string(udpPort_));
    } else {
      std::cerr << "[XRLinkDevice] FAILED to bind UDP port " << udpPort_ << std::endl;
      logToFile("[XRLinkDevice] FAILED to bind UDP port " + std::to_string(udpPort_));
    }
  }

  // 3. Setup TCP Server
  if (enableTcp_) {
    tcpServer_ = std::make_unique<QTcpServer>();
    if (tcpServer_->listen(QHostAddress::AnyIPv4, static_cast<quint16>(tcpPort_))) {
      QObject::connect(tcpServer_.get(), &QTcpServer::newConnection,
                       [this]() { onNewTcpConnection(); });
      std::cout << "[XRLinkDevice] TCP server listening on port " << tcpPort_ << std::endl;
      logToFile("[XRLinkDevice] TCP server listening on IPv4 port " + std::to_string(tcpPort_));
    }
  }
}

XRLinkDevice::~XRLinkDevice() { release(); }

namespace {
bool checkGlassesConnectivity(const QString& ip) {
  QProcess process;
  process.start(QStringLiteral("ping"), {QStringLiteral("-c"), QStringLiteral("1"), QStringLiteral("-W"), QStringLiteral("2"), ip});
  process.waitForFinished(3000);
  return process.exitCode() == 0;
}

QList<recordlab::core::UsbProductCatalogEntry> loadNvizUsbCatalog() {
#ifdef RECORDLABC_SOURCE_DIR
  const QString root = QString::fromUtf8(RECORDLABC_SOURCE_DIR);
#else
  const QString root = QCoreApplication::applicationDirPath();
#endif
  const QString configPath =
      QDir(root).filePath(QStringLiteral("config/recordLabConfig.json"));
  const auto result =
      recordlab::core::LegacyConfigLoader::loadRecordLabConfig(configPath);
  return result.success ? result.config.usbProductCatalog
                        : QList<recordlab::core::UsbProductCatalogEntry>{};
}

bool isPlaceholderFsn(const std::string &raw) {
  const QString value = QString::fromStdString(raw).trimmed();
  if (value.isEmpty()) {
    return true;
  }
  const QString lower = value.toLower();
  return lower == QStringLiteral("unknownfsn") ||
         lower == QStringLiteral("unknown_fsn") ||
         lower == QStringLiteral("unknown") ||
         lower == QStringLiteral("null") ||
         lower == QStringLiteral("none") ||
         lower == QStringLiteral("--");
}

std::optional<std::string> extractFsnFromText(const QString &rawOutput) {
  const QString output = rawOutput.trimmed();
  if (output.isEmpty() || !output.startsWith(QLatin1Char('{'))) {
    return std::nullopt;
  }

  try {
    const auto parsed = json::parse(output.toStdString());
    if (parsed.contains("FSN") && parsed["FSN"].is_string()) {
      const auto value = parsed["FSN"].get<std::string>();
      if (!isPlaceholderFsn(value)) {
        return value;
      }
    }
  } catch (...) {
  }

  return std::nullopt;
}

std::optional<std::string> readGlassesFsnViaSsh() {
  const QString sshpass = QStandardPaths::findExecutable(QStringLiteral("sshpass"));
  const QString ssh = QStandardPaths::findExecutable(QStringLiteral("ssh"));
  if (sshpass.isEmpty() || ssh.isEmpty()) {
    logToFile("[XRLinkDevice] sshpass or ssh not found; cannot read FSN via SSH");
    return std::nullopt;
  }

  const QStringList hosts = {QStringLiteral("169.254.2.1"),
                             QStringLiteral("169.254.1.1")};
  const QString command =
      QStringLiteral("cat /factory/glasses_config.json 2>/dev/null");

  for (const auto &host : hosts) {
    QStringList args;
    args << QStringLiteral("-p") << QStringLiteral("xreal2017")
         << ssh
         << QStringLiteral("-o") << QStringLiteral("BatchMode=no")
         << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=no")
         << QStringLiteral("-o") << QStringLiteral("UserKnownHostsFile=/dev/null")
         << QStringLiteral("-o") << QStringLiteral("ConnectTimeout=1")
         << QStringLiteral("-o") << QStringLiteral("HostKeyAlgorithms=+ssh-rsa,ssh-dss")
         << QStringLiteral("-o") << QStringLiteral("PubkeyAcceptedAlgorithms=+ssh-rsa,ssh-dss")
         << QStringLiteral("root@%1").arg(host)
         << command;

    QProcess process;
    process.start(sshpass, args);
    if (!process.waitForFinished(1500)) {
      process.kill();
      process.waitForFinished(300);
      continue;
    }
    if (process.exitCode() != 0) {
      continue;
    }

    if (auto fsn = extractFsnFromText(QString::fromUtf8(process.readAllStandardOutput()))) {
      logToFile("[XRLinkDevice] FSN read via SSH from " + host.toStdString());
      return fsn;
    }
  }

  return std::nullopt;
}
} // namespace

json XRLinkDevice::initialize(const json & /*params*/) {
  QString scriptPath = QDir(QString::fromUtf8(RECORDLABC_SOURCE_DIR)).filePath("subnodes/nviz_node/shell/close_pilot_gf.sh");
  logToFile("[XRLinkDevice::initialize] Running close_pilot_gf.sh");
  
  QProcess process;
  process.start(QStringLiteral("bash"), {scriptPath});
  if (process.waitForFinished(10000)) {
    logToFile("[XRLinkDevice::initialize] close script finished. code: " + std::to_string(process.exitCode()));
  } else {
    logToFile("[XRLinkDevice::initialize] close script timeout or failed");
  }

  logToFile("[XRLinkDevice::initialize] Waiting for glasses to disconnect...");
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 20000) {
    if (!checkGlassesConnectivity("169.254.1.1") && !checkGlassesConnectivity("169.254.2.1")) {
      logToFile("[XRLinkDevice::initialize] Glasses disconnected");
      break;
    }
    QThread::msleep(500);
  }

  logToFile("[XRLinkDevice::initialize] Waiting for glasses to be ready...");
  timer.restart();
  bool ready = false;
  while (timer.elapsed() < 20000) {
    if (checkGlassesConnectivity("169.254.1.1") || checkGlassesConnectivity("169.254.2.1")) {
      logToFile("[XRLinkDevice::initialize] Glasses ready after " + std::to_string(timer.elapsed()) + "ms");
      ready = true;
      break;
    }
    QThread::msleep(500);
  }

  if (!ready) {
    logToFile("[XRLinkDevice::initialize] Timeout waiting for glasses to be ready!");
    return {{"success", false}, {"message", "Timeout waiting for glasses"}};
  }

  return {{"success", true}, {"message", "Initialized"}};
}

namespace {
QString nvizShellScript(const QString &name) {
#ifdef RECORDLABC_SOURCE_DIR
  const QString root = QString::fromUtf8(RECORDLABC_SOURCE_DIR);
#else
  const QString root = QCoreApplication::applicationDirPath();
#endif
  return QDir(root).filePath(
      QStringLiteral("subnodes/nviz_node/shell/%1").arg(name));
}

nlohmann::json runNvizScript(const QString &script, int timeoutMs = 60000) {
  logToFile("[runNvizScript] Executing script: " + script.toStdString());
  QProcess process;
  process.start(QStringLiteral("bash"), {script});
  if (!process.waitForFinished(timeoutMs)) {
    process.kill();
    process.waitForFinished(1000);
    logToFile("[runNvizScript] Script timeout: " + script.toStdString());
    return {{"success", false},
            {"message", "script timeout: " + script.toStdString()}};
  }
  logToFile("[runNvizScript] Script finished. Exit code: " + std::to_string(process.exitCode()));
  logToFile("[runNvizScript] stdout: " + process.readAllStandardOutput().toStdString());
  logToFile("[runNvizScript] stderr: " + process.readAllStandardError().toStdString());
  return {{"success", process.exitCode() == 0},
          {"message", process.exitCode() == 0
                          ? std::string()
                          : process.readAllStandardError().toStdString()},
          {"stdout", process.readAllStandardOutput().toStdString()},
          {"stderr", process.readAllStandardError().toStdString()}};
}
} // namespace

json XRLinkDevice::start(const json &params) {
  logToFile("[XRLinkDevice::start] Called with params: " + params.dump());
  if (running_) {
    return {{"success", true}, {"message", "Already started"}};
  }
  const std::string dataType =
      params.value("data_type", std::string("3dof"));
  const bool recordMode = params.value("record", false);
  QString scriptName;
  if (dataType == "3dof") {
    scriptName = recordMode ? QStringLiteral("gf_3dof_start_record.sh")
                            : QStringLiteral("gf_3dof_start.sh");
  } else if (dataType == "6dof") {
    scriptName = recordMode ? QStringLiteral("open_pilot_gf.sh")
                            : QStringLiteral("open_sdk_global_shell");
  } else {
    logToFile("[XRLinkDevice::start] Invalid data_type: " + dataType);
    return {{"success", false},
            {"message", "Invalid data_type: " + dataType}};
  }
  
  QString scriptPath = nvizShellScript(scriptName);
  logToFile("[XRLinkDevice::start] Resolved script path: " + scriptPath.toStdString());
  if (!QFile::exists(scriptPath)) {
    logToFile("[XRLinkDevice::start] Script DOES NOT EXIST!");
  }

  auto scriptResult = runNvizScript(scriptPath);
  if (!scriptResult.value("success", false)) {
    logToFile("[XRLinkDevice::start] Script execution failed!");
    return scriptResult;
  }
  running_ = true;
  std::cout << "[XRLinkDevice] Device started (data_type=" << dataType << ")" << std::endl;
  logToFile("[XRLinkDevice::start] Device started successfully.");
  return {{"success", true}, {"message", ""}};
}

json XRLinkDevice::stop(const json &params) {
  const std::string dataType =
      params.value("data_type", std::string("3dof"));
  const bool recordMode = params.value("record", false);
  QString scriptName;
  if (dataType == "3dof") {
    scriptName = recordMode ? QStringLiteral("gf_3dof_end_record.sh")
                            : QStringLiteral("gf_3dof_end.sh");
  } else if (dataType == "6dof") {
    scriptName = recordMode ? QStringLiteral("close_pilot_gf.sh")
                            : QStringLiteral("close_pilot_shell");
  } else {
    return {{"success", false},
            {"message", "Invalid data_type: " + dataType}};
  }
  auto scriptResult = runNvizScript(nvizShellScript(scriptName));
  if (!scriptResult.value("success", false)) {
    return scriptResult;
  }
  running_ = false;
  return {{"success", true}, {"message", ""}};
}

json XRLinkDevice::release() {
  running_ = false;
  if (udpSocket_) {
    udpSocket_->disconnect();
    udpSocket_->close();
    udpSocket_.reset();
  }
  if (tcpServer_) {
    tcpServer_->disconnect();
    tcpServer_->close();
    tcpServer_.reset();
  }
  for (auto *client : tcpClients_) {
    if (client) {
      client->disconnect();
      client->deleteLater();
    }
  }
  tcpClients_.clear();
  return {{"success", true}, {"message", ""}};
}

json XRLinkDevice::check() {
  try {
    const auto catalog = loadNvizUsbCatalog();
    const auto devices = recordlab::core::detectUsbProducts(catalog);
    const auto selected = recordlab::core::chooseUsbProductForAgent(
        devices, QStringLiteral("glasses_nviz_node"));
    if (!selected.has_value()) {
      return {{"success", false},
              {"message", "No known NViz USB glasses connected"},
              {"product_ids", json::array()},
              {"device_count", 0},
              {"fsn", ""}};
    }

    json productIds = json::array();
    for (const auto &device : devices) {
      if (device.catalogEntry.agentName == QStringLiteral("glasses_nviz_node")) {
        productIds.push_back(device.catalogEntry.pid);
      }
    }

    const auto &product = selected->catalogEntry;
    json result = {{"success", true},
                   {"message", ""},
                   {"product_ids", productIds},
                   {"product_id", product.pid},
                   {"device_count", static_cast<int>(productIds.size())},
                   {"usb_vid", selected->vidHex.toStdString()},
                   {"usb_pid", selected->pidHex.toStdString()},
                   {"product_display_id", selected->pidHex.toStdString()},
                   {"product_name", product.displayName.toStdString()},
                   {"fsn", ""}};
    if (auto fsn = readGlassesFsnViaSsh()) {
      result["fsn"] = *fsn;
    }
    return result;
  } catch (const std::exception &e) {
    return {{"success", false},
            {"message", e.what()},
            {"product_ids", json::array()},
            {"device_count", 0},
            {"fsn", ""}};
  }
}

void XRLinkDevice::onUdpData() {
  static uint64_t udpDatagramCount = 0;
  while (udpSocket_ && udpSocket_->hasPendingDatagrams()) {
    QByteArray datagram;
    datagram.resize(static_cast<int>(udpSocket_->pendingDatagramSize()));
    udpSocket_->readDatagram(datagram.data(), datagram.size());
    udpDatagramCount++;
    if (udpDatagramCount <= 50 || udpDatagramCount % 1000 == 0) {
      logToFile("[XRLinkDevice][UDP] datagram_count=" +
                std::to_string(udpDatagramCount) +
                " size=" + std::to_string(datagram.size()) +
                " running=" + std::to_string(running_));
    }
    if (running_) {
      processSensorDatagram(datagram);
    }
  }
}

void XRLinkDevice::onNewTcpConnection() {
  logToFile("[XRLinkDevice] onNewTcpConnection triggered!");
  while (tcpServer_ && tcpServer_->hasPendingConnections()) {
    QTcpSocket *socket = tcpServer_->nextPendingConnection();
    std::string clientInfo = socket->peerAddress().toString().toStdString() + ":" + std::to_string(socket->peerPort());
    std::cout << "[XRLinkDevice] New TCP connection from " << clientInfo << std::endl;
    logToFile("[XRLinkDevice] New TCP connection from " + clientInfo);

    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    socket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, 256 * 1024);
    socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 512 * 1024);

    auto *client = new NrealLinkTcpClient(socket, [this](const QByteArray &data) {
      if (running_) {
        processSensorDatagram(data);
      }
    }, nullptr);

    QObject::connect(client, &QObject::destroyed, [this, client]() {
      auto it = std::find(tcpClients_.begin(), tcpClients_.end(), client);
      if (it != tcpClients_.end()) {
        tcpClients_.erase(it);
      }
    });

    tcpClients_.push_back(client);
  }
}

void XRLinkDevice::processSensorDatagram(const QByteArray &sensorData) {
  static uint64_t sensorDatagramCount = 0;
  sensorDatagramCount++;
  const int size = sensorData.size();
  constexpr int headerSize = static_cast<int>(sizeof(NrealLinkMsgHeader));

  if (size <= headerSize) {
    logToFile("[XRLinkDevice] Drop short datagram, size=" + std::to_string(size));
    return;
  }

  NrealLinkMsgHeader header;
  std::memcpy(&header, sensorData.constData(), headerSize);

  if (header.payload_length + headerSize != size) {
    logToFile("[XRLinkDevice] Drop malformed datagram, payload_length=" +
              std::to_string(header.payload_length) +
              ", size=" + std::to_string(size));
    return;
  }

  const int groupId = static_cast<int>(header.magic);
  const int msgId = header.msg_id;
  if (sensorDatagramCount <= 100 || sensorDatagramCount % 1000 == 0) {
    logToFile("[XRLinkDevice][SENSOR_DATAGRAM] count=" +
              std::to_string(sensorDatagramCount) +
              " group=" + std::to_string(groupId) +
              " msg=" + std::to_string(msgId) +
              " payload_length=" + std::to_string(header.payload_length) +
              " size=" + std::to_string(size));
  }

  const int payloadStart = headerSize - 8;
  const int payloadLen = header.payload_length + 8;
  if (payloadStart < 0 || payloadStart + payloadLen > size) {
    logToFile("[XRLinkDevice] Drop datagram with invalid payload window");
    return;
  }

  QByteArray payload = sensorData.mid(payloadStart, payloadLen);
  dispatchParsedPayload(groupId, msgId, payload);
}

void XRLinkDevice::dispatchParsedPayload(int groupId, int msgId,
                                         const QByteArray &payload) {
  static uint64_t dispatchCount = 0;
  dispatchCount++;
  maybePublishTreeData(groupId, msgId, payload);
  // 3dof mode currently uses group 126/127 msg 1 for IMU plus group 1 msg 1
  // for link delay. Ignore the rest early; some TCP bursts contain thousands of
  // packets, and parsing unknown payloads on the hot path can destabilize the
  // subnode without helping the UI.
  if (!((groupId == 1 && msgId == 1) ||
        ((groupId == 126 || groupId == 127) && msgId == 1))) {
    if (dispatchCount <= 100 || dispatchCount % 1000 == 0) {
      logToFile("[XRLinkDevice][DISPATCH_SKIP] count=" +
                std::to_string(dispatchCount) +
                " group=" + std::to_string(groupId) +
                " msg=" + std::to_string(msgId) +
                " payload=" + std::to_string(payload.size()));
    }
    return;
  }

  auto parsed = payloadParser_.parse(groupId, msgId, payload);
  if (parsed.size() < 10 &&
      ((groupId == 1 && msgId == 1) ||
       ((groupId == 126 || groupId == 127) && msgId == 1))) {
    parsed = parseStandardNrealPayload(payload);
  }
  if (dispatchCount <= 100 || dispatchCount % 1000 == 0) {
    logToFile("[XRLinkDevice][PARSED] count=" +
              std::to_string(dispatchCount) +
              " group=" + std::to_string(groupId) +
              " msg=" + std::to_string(msgId) +
              " payload=" + std::to_string(payload.size()) +
              " values=" + std::to_string(parsed.size()));
  }
  if (parsed.empty()) {
    logToFile("[XRLinkDevice] Parsed empty payload (group=" +
              std::to_string(groupId) + ", msg=" + std::to_string(msgId) +
              ", payload=" + std::to_string(payload.size()) + ")");
    return;
  }

  packetCount_++;
  if (packetCount_ == 1) {
    logToFile("[XRLinkDevice] FIRST packet processed! (group=" + std::to_string(groupId) + ", msg=" + std::to_string(msgId) + ")");
  }
  if (packetCount_ % 10000 == 1) {
    std::cout << "[XRLinkDevice] Packets processed: " << packetCount_
              << " (group=" << groupId << ", msg=" << msgId << ")" << std::endl;
  }

  if (groupId == 1 && msgId == 1) {
    if (auto delayMsg = buildTimeDelayMessage(parsed); delayMsg.has_value()) {
      logToFile("[XRLinkDevice] nreal_link time_delay_ns=" +
                std::to_string(delayMsg->value("time_delay_ns", 0LL)));
      if (timeDelayCallback_) {
        logToFile("[XRLinkDevice][TIME_DELAY_CALLBACK] " + delayMsg->dump());
        timeDelayCallback_(delayMsg.value());
      }
    }
    return;
  }

  if (parsed.size() >= 10 && imuCallback_) {
    static ImuSourceProbe sourceProbe;
    int imuType = static_cast<int>(parsed[3]);
    if (imuType >= 1 && imuType <= 13) {
      logImuSourceProbe("XRLinkDevice::dispatchParsedPayload", sourceProbe,
                        steadyNowNs(), static_cast<int64_t>(parsed[2]),
                        imuType, groupId, msgId);
      json imuMessage = {
          {"type", imuType},
          {"timestamp_ns", static_cast<int64_t>(parsed[2])},
          {"data",
           json::array({parsed.size() > 4 ? parsed[4] : 0.0,
                        parsed.size() > 5 ? parsed[5] : 0.0,
                        parsed.size() > 6 ? parsed[6] : 0.0,
                        parsed.size() > 7 ? parsed[7] : 0.0,
                        parsed.size() > 8 ? parsed[8] : 0.0,
                        parsed.size() > 9 ? parsed[9] : 0.0})}};
      static uint64_t imuCallbackCount = 0;
      imuCallbackCount++;
      if (imuCallbackCount <= 100 || imuCallbackCount % 1000 == 0) {
        logToFile("[XRLinkDevice][IMU_CALLBACK] count=" +
                  std::to_string(imuCallbackCount) +
                  " group=" + std::to_string(groupId) +
                  " msg=" + std::to_string(msgId) +
                  " imu_type=" + std::to_string(imuType) +
                  " payload=" + imuMessage.dump());
      }
      imuCallback_(imuMessage);
    }
  }
}

std::optional<json>
XRLinkDevice::buildTimeDelayMessage(const std::vector<double> &parsed) const {
  if (parsed.size() < 6) {
    return std::nullopt;
  }
  const int64_t timestampNs = static_cast<int64_t>(parsed[2]);
  const auto sendQueueDelayNs =
      static_cast<int64_t>(parsed[5] * 1'000'000'000.0 + 1.0);
  return json{{"name", "time_delay"},
              {"timestamp_ns", timestampNs},
              {"time_delay_ns", sendQueueDelayNs},
              {"status", "valid"}};
}

void XRLinkDevice::maybePublishTreeData(int groupId, int msgId,
                                        const QByteArray &payload) {
  static uint64_t noCallbackCount = 0;
  static uint64_t noTemplateCount = 0;
  static uint64_t treePublishCount = 0;
  if (!treeDataCallback_) {
    ++noCallbackCount;
    if (noCallbackCount <= 5 || noCallbackCount % 1000 == 0) {
      std::cerr << "[XRLinkDevice][NVIZ_TREE] no tree callback, group="
                << groupId << " msg=" << msgId << std::endl;
    }
    return;
  }
  if (!payloadParser_.hasTemplate(groupId, msgId)) {
    ++noTemplateCount;
    if (noTemplateCount <= 20 || noTemplateCount % 1000 == 0) {
      std::cerr << "[XRLinkDevice][NVIZ_TREE] no plot.json template, group="
                << groupId << " msg=" << msgId
                << " payload=" << payload.size() << std::endl;
    }
    return;
  }

  const int64_t key =
      (static_cast<int64_t>(groupId) << 32) | static_cast<int64_t>(msgId);
  const int64_t nowNs = steadyNowNs();

  auto &arrivals = treeArrivalNs_[key];
  arrivals.push_back(nowNs);
  constexpr int64_t kRateWindowNs = 1'000'000'000LL;
  while (!arrivals.empty() && arrivals.front() < nowNs - kRateWindowNs) {
    arrivals.pop_front();
  }
  double rawFrequencyHz = 0.0;
  if (arrivals.size() > 1) {
    const double spanSec =
        static_cast<double>(arrivals.back() - arrivals.front()) / 1e9;
    rawFrequencyHz =
        spanSec > 0.001 ? static_cast<double>(arrivals.size() - 1) / spanSec
                        : 0.0;
  }

  auto lastIt = lastTreePublishNs_.find(key);
  if (lastIt != lastTreePublishNs_.end() &&
      nowNs - lastIt->second < 50'000'000LL) {
    return;
  }
  lastTreePublishNs_[key] = nowNs;

  auto fields = payloadParser_.parseFields(groupId, msgId, payload);
  if (fields.empty()) {
    std::cerr << "[XRLinkDevice][NVIZ_TREE] parsed empty fields, group="
              << groupId << " msg=" << msgId
              << " payload=" << payload.size() << std::endl;
    return;
  }

  json fieldArray = json::array();
  for (const auto &field : fields) {
    fieldArray.push_back(
        {{"name", field.name}, {"type", field.type}, {"value", field.value}});
  }

  treeDataCallback_(
      json{{"group_id", groupId},
           {"msg_id", msgId},
           {"message_name", payloadParser_.messageName(groupId, msgId)},
           {"timestamp", static_cast<double>(nowNs) / 1e9},
           {"frequency_hz", rawFrequencyHz},
           {"fields", std::move(fieldArray)}});
  ++treePublishCount;
  if (treePublishCount <= 20 || treePublishCount % 1000 == 0) {
    std::cerr << "[XRLinkDevice][NVIZ_TREE] published group=" << groupId
              << " msg=" << msgId << " fields=" << fields.size()
              << " count=" << treePublishCount << std::endl;
  }
}

// ============================================================================
// NvizNodeSubnode
// ============================================================================

NvizNodeSubnode::NvizNodeSubnode(const QString &name,
                                 const QString &subnodeHost, int goalPort,
                                 int feedbackPort, const QString &rootPath,
                                 const QString &deviceHost, int udpPort,
                                 int tcpPort, bool enableUdp, bool enableTcp,
                                 QObject *parent)
    : MainSubnode(name, subnodeHost, goalPort, feedbackPort, rootPath, nullptr,
                  parent) {
  logToFile("[NvizNodeSubnode][CTOR] name=" + name.toStdString() +
            " host=" + subnodeHost.toStdString() +
            " goalPort=" + std::to_string(goalPort) +
            " feedbackPort=" + std::to_string(feedbackPort) +
            " rootPath=" + rootPath.toStdString() +
            " deviceHost=" + deviceHost.toStdString() +
            " udpPort=" + std::to_string(udpPort) +
            " tcpPort=" + std::to_string(tcpPort) +
            " enableUdp=" + std::to_string(enableUdp) +
            " enableTcp=" + std::to_string(enableTcp));
  xrLinkDevice_ = std::make_unique<XRLinkDevice>(deviceHost.toStdString(),
                                                 udpPort, tcpPort,
                                                 enableUdp, enableTcp);
  device_ = xrLinkDevice_.get();
  device_->setImuDataCallback(
      [this](const json &msg) { imuDataCallback(msg); });
  device_->setImageDataCallback(
      [this](const json &msg) { imageDataCallback(msg); });
  xrLinkDevice_->setTimeDelayCallback([this](const json &msg) {
    logToFile("[NvizNodeSubnode][PUBLISH_TIME_DELAY] " + msg.dump());
    publish(recordlab::common::PORT_TIME_DELAY, msg);
  });
  xrLinkDevice_->setTreeDataCallback([this](const json &msg) {
    publish(recordlab::common::PORT_NVIZ_TREE, msg);
  });

  std::cout << "[" << name.toStdString() << "] NvizNodeSubnode initialized"
            << std::endl;
}

NvizNodeSubnode::~NvizNodeSubnode() = default;

int nvizNodeSubnodeMain(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);

  auto signalHandler = [](int signum) {
    std::cout << "Received signal " << signum << ", shutting down..."
              << std::endl;
    QCoreApplication::quit();
  };
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  QCommandLineParser parser;
  parser.addOption({"name", "Node name", "glasses_nviz_node"});
  parser.addOption({"host", "Host address", "127.0.0.1"});
  parser.addOption({"goal-port", "Goal port", "5692"});
  parser.addOption({"feedback-port", "Feedback port", "5693"});
  parser.addOption({"root-path", "Data root path", "./output"});
  parser.addOption({"device-host", "Device host", "127.0.0.1"});
  parser.addOption({"udp-port", "UDP port", "7099"});
  parser.addOption({"tcp-port", "TCP port", "8099"});
  parser.addOption(QCommandLineOption("enable-udp", "Enable UDP mode"));
  parser.addOption(QCommandLineOption("enable-tcp", "Enable TCP mode"));
  parser.process(app);

  QString name = parser.value("name");
  if (name.isEmpty()) name = "glasses_nviz_node";
  QString host = parser.value("host");
  if (host.isEmpty()) host = "127.0.0.1";
  int goalPort = parser.value("goal-port").toInt();
  if (goalPort == 0) goalPort = 5692;
  int feedbackPort = parser.value("feedback-port").toInt();
  if (feedbackPort == 0) feedbackPort = 5693;
  QString rootPath = parser.value("root-path");
  if (rootPath.isEmpty()) rootPath = "./output";
  QString deviceHost = parser.value("device-host");
  if (deviceHost.isEmpty()) deviceHost = "127.0.0.1";
  int udpPort = parser.value("udp-port").toInt();
  if (udpPort == 0) udpPort = 7099;
  int tcpPort = parser.value("tcp-port").toInt();
  if (tcpPort == 0) tcpPort = 8099;
  
  bool enableUdp = parser.isSet("enable-udp");
  bool enableTcp = parser.isSet("enable-tcp");
  
  // 默认启用 UDP（如果都没设置的话），兼容之前的行为
  if (!enableUdp && !enableTcp) {
      enableUdp = true;
  }

  logToFile("[NvizNodeSubnodeMain][ARGS] name=" + name.toStdString() +
            " host=" + host.toStdString() +
            " goalPort=" + std::to_string(goalPort) +
            " feedbackPort=" + std::to_string(feedbackPort) +
            " rootPath=" + rootPath.toStdString() +
            " deviceHost=" + deviceHost.toStdString() +
            " udpPort=" + std::to_string(udpPort) +
            " tcpPort=" + std::to_string(tcpPort) +
            " enableUdp=" + std::to_string(enableUdp) +
            " enableTcp=" + std::to_string(enableTcp));

  QTimer ticker;
  QObject::connect(&ticker, &QTimer::timeout, []() {});
  ticker.start(500);

  NvizNodeSubnode subnode(name, host, goalPort, feedbackPort, rootPath,
                          deviceHost, udpPort, tcpPort, enableUdp, enableTcp);
  subnode.createMainPublishers();

  auto result = subnode.connect();
  if (!result.value("success", false)) {
    std::cerr << "Failed to connect: "
              << result.value("message", std::string("Unknown error"))
              << std::endl;
    return 1;
  }

  std::cout << "[" << name.toStdString() << "] SubNode ready (UDP=" 
            << (enableUdp ? "ON" : "OFF") << ", TCP=" << (enableTcp ? "ON" : "OFF") 
            << ")" << std::endl;
  return app.exec();
}

} // namespace recordlab::subnodes
