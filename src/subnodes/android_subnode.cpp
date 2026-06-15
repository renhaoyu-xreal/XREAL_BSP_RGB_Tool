#include "recordlab/subnodes/android_subnode.h"

#include "recordlab/common/topics.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QMetaObject>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>

namespace recordlab::subnodes {

using json = nlohmann::json;
using namespace recordlab::common;

namespace {

constexpr int kAndroidTcpPort = 8100;
constexpr int kMobileGroupId = 126;
constexpr int kMobileMsgId = 1;
constexpr int kMobilePayloadSize = 8 + 8 + 8 + 4 + 6 * 4;
constexpr uint8_t kTcpMagicNum = 239;
constexpr uint32_t kTcpMaxMsgCount = 50000;
constexpr int kTcpMaxFrameLength = 8 * 1024 * 1024;

QString projectRootPath() {
  const QString envRoot = qEnvironmentVariable("RECORDLABC_ROOT").trimmed();
  if (!envRoot.isEmpty() && QFileInfo(envRoot).isDir()) {
    return QDir::cleanPath(envRoot);
  }
#ifdef RECORDLABC_SOURCE_DIR
  const QString compiledRoot = QString::fromUtf8(RECORDLABC_SOURCE_DIR);
  if (QFileInfo(compiledRoot).isDir()) {
    return QDir::cleanPath(compiledRoot);
  }
#endif
  return QDir::cleanPath(QCoreApplication::applicationDirPath());
}

QString resolveStorageRootPath(const QString &configuredRootPath) {
  const QString cleaned = QDir::cleanPath(configuredRootPath.trimmed());
  if (cleaned.isEmpty()) {
    return cleaned;
  }
  if (QDir::isAbsolutePath(cleaned)) {
    return cleaned;
  }
  return QDir(projectRootPath()).filePath(cleaned);
}

template <typename T>
T readLe(const uint8_t *raw, int &offset) {
  T value{};
  std::memcpy(&value, raw + offset, sizeof(T));
  offset += static_cast<int>(sizeof(T));
  return value;
}

QString processOutput(QProcess &process) {
  const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
  const QString stdoutText = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
  return !stderrText.isEmpty() ? stderrText : stdoutText;
}

QString fanCycleScript() {
  return QStringLiteral(R"(#!/system/bin/sh
trap 'echo 0 > /sys/class/fan/max31760/speed_control 2>/dev/null; echo 0 > /sys/class/fan/max31760/speed_fixed_flag 2>/dev/null; exit 0' INT TERM HUP EXIT
echo 1 > /sys/class/fan/max31760/speed_fixed_flag 2>/dev/null
while :; do
  echo 0 > /sys/class/fan/max31760/speed_control 2>/dev/null
  sleep 60
  echo 100 > /sys/class/fan/max31760/speed_control 2>/dev/null
  sleep 60
done
)");
}

QString gpsCycleScript() {
  return QStringLiteral(R"(#!/system/bin/sh
gps_on() {
  echo 1 > /sys/class/fan/max31760/speed_fixed_flag 2>/dev/null
  echo 0 > /sys/class/fan/max31760/speed_control 2>/dev/null
  cmd location set-location-enabled true 2>/dev/null
  settings put secure location_providers_allowed +gps 2>/dev/null
  dumpsys location 2>/dev/null | grep -E "GnssStatus|num_sv|Satellite" | grep -v "null" | head -n 5
}

gps_off() {
  echo 1 > /sys/class/fan/max31760/speed_fixed_flag 2>/dev/null
  echo 0 > /sys/class/fan/max31760/speed_control 2>/dev/null
  cmd location set-location-enabled false 2>/dev/null
  cmd location is-location-enabled 2>/dev/null
}

trap 'gps_off; echo 0 > /sys/class/fan/max31760/speed_control 2>/dev/null; echo 0 > /sys/class/fan/max31760/speed_fixed_flag 2>/dev/null; exit 0' INT TERM HUP EXIT

while :; do
  gps_on
  sleep 60
  gps_off
  sleep 60
done
)");
}

QString loadScript() {
  return QStringLiteral(R"(#!/system/bin/sh
MODE="${1:-high}"
CPU_PIDS=""
MEM_PIDS=""
IO_PIDS=""
DDR_PATH="/sys/devices/system/cpu/bus_dcvs/DDR/19091000.qcom,bwmon-ddr"
DISK_TARGET="/data/local/tmp/stress_test.img"

get_cores() {
  cores=$(nproc 2>/dev/null)
  if [ -z "$cores" ]; then
    cores=$(grep -c '^processor' /proc/cpuinfo 2>/dev/null)
  fi
  if [ -z "$cores" ] || [ "$cores" -lt 1 ]; then
    cores=4
  fi
  echo "$cores"
}

start_high_load() {
  cores=$(get_cores)
  cores_used=$((cores - 1))
  if [ "$cores_used" -lt 1 ]; then
    cores_used=1
  fi

  i=1
  while [ "$i" -le "$cores_used" ]; do
    sh -c 'while :; do for i in $(seq 1 100); do head -c 1M /dev/urandom | md5sum >/dev/null 2>&1; sleep 0.001; done; done' recordlab_cpu_load &
    CPU_PIDS="$CPU_PIDS $!"
    i=$((i + 1))
  done

  sh -c '
    while :; do
      echo 547200 > '"$DDR_PATH"'/min_freq 2>/dev/null
      echo 547200 > '"$DDR_PATH"'/max_freq 2>/dev/null
      cat '"$DDR_PATH"'/cur_freq >/dev/null 2>&1
      for i in $(seq 1 10); do
        cat /dev/urandom | head -c 100M | tail >/dev/null
        sleep 0.001
      done
      sleep 10
      echo 3196800 > '"$DDR_PATH"'/min_freq 2>/dev/null
      echo 3196800 > '"$DDR_PATH"'/max_freq 2>/dev/null
      cat '"$DDR_PATH"'/cur_freq >/dev/null 2>&1
      for i in $(seq 1 10); do
        cat /dev/urandom | head -c 100M | tail >/dev/null
        sleep 0.001
      done
    done
  ' recordlab_mem_load &
  MEM_PIDS="$MEM_PIDS $!"

  sh -c '
    target="'"$DISK_TARGET"'"
    while :; do
      for i in $(seq 1 20); do
        dd if=/dev/zero of="$target" bs=1M count=1024 conv=fsync >/dev/null 2>&1
        rm -f "$target"
        sleep 0.001
      done
    done
  ' recordlab_io_load &
  IO_PIDS="$IO_PIDS $!"
}

stop_high_load() {
  for pid in $CPU_PIDS $MEM_PIDS $IO_PIDS; do
    kill "$pid" 2>/dev/null
  done
  pkill -f recordlab_cpu_load 2>/dev/null
  pkill -f recordlab_mem_load 2>/dev/null
  pkill -f recordlab_io_load 2>/dev/null
  rm -f "$DISK_TARGET" /data/local/tmp/recordlab_stress_*.img
  CPU_PIDS=""
  MEM_PIDS=""
  IO_PIDS=""
}

cleanup() {
  trap - INT TERM HUP EXIT
  stop_high_load
  exit 0
}

trap cleanup INT TERM HUP EXIT

if [ "$MODE" = "wave" ]; then
  while :; do
    start_high_load
    sleep 60
    stop_high_load
    sleep 60
  done
else
  start_high_load
  while :; do
    sleep 3600
  done
fi
)");
}

} // namespace

uint8_t AndroidTcpClient::crc8(const uint8_t *data, int size) {
  uint8_t crc = 0x00;
  constexpr uint8_t poly = 0x07;
  for (int i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; ++j) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ poly)
                         : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

AndroidTcpClient::AndroidTcpClient(QTcpSocket *socket, DataCallback callback,
                                   ParseEventCallback parseEventCallback,
                                   QObject *parent)
    : QObject(parent), socket_(socket), callback_(std::move(callback)),
      parseEventCallback_(std::move(parseEventCallback)) {
  if (socket_) {
    socket_->setParent(this);
  }
  connect(socket_, &QTcpSocket::readyRead, this, &AndroidTcpClient::onReadyRead);
  connect(socket_, &QTcpSocket::disconnected, this,
          &AndroidTcpClient::onDisconnected);
}

AndroidTcpClient::~AndroidTcpClient() {
  if (socket_) {
    socket_->disconnect(this);
    socket_->disconnectFromHost();
    socket_->deleteLater();
    socket_ = nullptr;
  }
}

void AndroidTcpClient::onDisconnected() {
  std::cout << "[AndroidTcpClient] Client disconnected" << std::endl;
  deleteLater();
}

void AndroidTcpClient::onReadyRead() {
  if (handlingReadyRead_) {
    readyReadAgain_ = true;
    return;
  }

  handlingReadyRead_ = true;
  do {
    readyReadAgain_ = false;
    while (socket_ && socket_->bytesAvailable() > 0) {
      QByteArray data = socket_->readAll();
      if (data.isEmpty()) {
        continue;
      }

      buffer_.append(data);
      reportParseEvent(QStringLiteral("ready_read"));
      reportParseEvent(QStringLiteral("read_bytes"),
                       static_cast<uint64_t>(data.size()));
      reportParseEvent(QStringLiteral("tcp_buffer_size"),
                       static_cast<uint64_t>(buffer_.size()));
      parseStream();
    }
  } while (readyReadAgain_ && socket_ && socket_->bytesAvailable() > 0);
  handlingReadyRead_ = false;
}

void AndroidTcpClient::parseStream() {
  while (!buffer_.isEmpty()) {
    if (status_ == INIT || status_ == BODY_PARSED) {
      if (!parseHead()) {
        break;
      }
      continue;
    }
    if (status_ == HEAD_PARSED) {
      if (!parseBody()) {
        break;
      }
      continue;
    }
    break;
  }
}

bool AndroidTcpClient::parseHead() {
  const int frameSize = 1 + static_cast<int>(sizeof(AndroidTcpMsgHeader));
  if (buffer_.size() < frameSize) {
    return false;
  }

  auto isValidTcpHeader = [](const AndroidTcpMsgHeader &header) {
    if (header.magic_num != kTcpMagicNum || header.msg_count > kTcpMaxMsgCount ||
        header.length < sizeof(AndroidTcpMsgHeader) ||
        header.length > kTcpMaxFrameLength) {
      return false;
    }
    AndroidTcpMsgHeader headerForCrc = header;
    headerForCrc.crc = 0;
    const uint8_t computed =
        AndroidTcpClient::crc8(reinterpret_cast<const uint8_t *>(&headerForCrc),
                               sizeof(AndroidTcpMsgHeader));
    return header.crc == computed;
  };

  int headerOffset = -1;
  AndroidTcpMsgHeader header{};
  for (int i = 0; i + frameSize <= buffer_.size(); ++i) {
    const uint8_t packetHeaderByte = static_cast<uint8_t>(buffer_.at(i));
    if ((packetHeaderByte & 0x01) != 0) {
      continue;
    }
    std::memcpy(&header, buffer_.constData() + i + 1,
                sizeof(AndroidTcpMsgHeader));
    if (isValidTcpHeader(header)) {
      headerOffset = i;
      break;
    }
  }

  if (headerOffset < 0) {
    const int keepBytes = frameSize - 1;
    const int dropBytes =
        std::max(1, static_cast<int>(buffer_.size()) - keepBytes);
    std::cerr << "[AndroidTcpClient] TCP header parse failed: buffer_size="
              << buffer_.size() << " dropped_bytes=" << dropBytes
              << std::endl;
    buffer_.remove(0, dropBytes);
    reportParseEvent(QStringLiteral("header"));
    reportParseEvent(QStringLiteral("resync"));
    reportParseEvent(QStringLiteral("dropped_bytes"),
                     static_cast<uint64_t>(dropBytes));
    return false;
  }

  if (headerOffset > 0) {
    buffer_.remove(0, headerOffset);
    reportParseEvent(QStringLiteral("resync"));
    reportParseEvent(QStringLiteral("dropped_bytes"),
                     static_cast<uint64_t>(headerOffset));
  }
  curHeader_ = header;

  if ((static_cast<uint8_t>(buffer_.at(0)) & 0x01) != 0) {
    buffer_.remove(0, 1);
    reportParseEvent(QStringLiteral("header"));
    reportParseEvent(QStringLiteral("resync"));
    reportParseEvent(QStringLiteral("dropped_bytes"), 1);
    return false;
  }

  AndroidTcpMsgHeader headerForCrc = curHeader_;
  headerForCrc.crc = 0;
  const uint8_t computed =
      crc8(reinterpret_cast<const uint8_t *>(&headerForCrc),
           sizeof(AndroidTcpMsgHeader));
  if (curHeader_.crc != computed) {
    std::cerr << "[AndroidTcpClient] TCP header CRC failed: buffer_size="
              << buffer_.size() << " received="
              << static_cast<int>(curHeader_.crc) << " computed="
              << static_cast<int>(computed) << std::endl;
    buffer_.remove(0, 1);
    reportParseEvent(QStringLiteral("header"));
    reportParseEvent(QStringLiteral("resync"));
    reportParseEvent(QStringLiteral("dropped_bytes"), 1);
    return false;
  }

  buffer_.remove(0, frameSize);
  bodyBuffer_.clear();
  status_ = HEAD_PARSED;
  if (curHeader_.timestamp_ns == lastSuccessTimestamp_) {
    const int bodySize = static_cast<int>(curHeader_.length) -
                         static_cast<int>(sizeof(AndroidTcpMsgHeader));
    if (buffer_.size() >= 1 + bodySize &&
        (static_cast<uint8_t>(buffer_.at(0)) & 0x01) == 1) {
      buffer_.remove(0, 1 + bodySize);
    }
    respondToClient();
    status_ = BODY_PARSED;
    return false;
  }
  respondToClient();
  return true;
}

bool AndroidTcpClient::parseBody() {
  const int bodySize = static_cast<int>(curHeader_.length) -
                       static_cast<int>(sizeof(AndroidTcpMsgHeader));
  if (bodySize <= 0) {
    status_ = BODY_PARSED;
    return false;
  }
  while (bodyBuffer_.size() < bodySize) {
    if (buffer_.isEmpty()) {
      respondToClient();
      return false;
    }

    const uint8_t packetHeaderByte = static_cast<uint8_t>(buffer_.at(0));
    if ((packetHeaderByte & 0x01) != 1) {
      std::cerr << "[AndroidTcpClient] TCP body prefix failed: bodySize="
                << bodySize << " body_buffer_size=" << bodyBuffer_.size()
                << " stream_buffer_size=" << buffer_.size()
                << " prefix=" << static_cast<int>(packetHeaderByte)
                << std::endl;
      buffer_.remove(0, 1);
      bodyBuffer_.clear();
      status_ = BODY_PARSED;
      respondToClient();
      reportParseEvent(QStringLiteral("body"));
      reportParseEvent(QStringLiteral("resync"));
      reportParseEvent(QStringLiteral("dropped_bytes"), 1);
      return false;
    }

    buffer_.remove(0, 1);
    const int needed = bodySize - static_cast<int>(bodyBuffer_.size());
    const int chunkSize = std::min(needed, static_cast<int>(buffer_.size()));
    if (chunkSize > 0) {
      bodyBuffer_.append(buffer_.constData(), chunkSize);
      buffer_.remove(0, chunkSize);
    }
  }

  int offset = 0;
  std::vector<QByteArray> sensorPackets;
  const QByteArray body = bodyBuffer_.left(bodySize);

  while (offset < bodySize) {
    if (offset + static_cast<int>(sizeof(AndroidNrealLinkMsgHeader)) >
        body.size()) {
      break;
    }

    AndroidNrealLinkMsgHeader header{};
    std::memcpy(&header, body.constData() + offset,
                sizeof(AndroidNrealLinkMsgHeader));

    const int packetSize =
        static_cast<int>(sizeof(AndroidNrealLinkMsgHeader)) +
        header.payload_length;
    if (header.payload_length <= 0 || packetSize <= 0 ||
        offset + packetSize > bodySize) {
      std::cerr << "[AndroidTcpClient] TCP body parse failed: bodySize="
                << bodySize << " buffer_size=" << buffer_.size()
                << " msg_count=" << curHeader_.msg_count
                << " parsed_packets=" << sensorPackets.size()
                << " logical_offset=" << offset
                << " magic=" << static_cast<int>(header.magic)
                << " msg_id=" << header.msg_id
                << " payload_length=" << header.payload_length << std::endl;
      if (!bodyBuffer_.isEmpty()) {
        bodyBuffer_.remove(0, 1);
        buffer_.prepend(bodyBuffer_);
        bodyBuffer_.clear();
      }
      status_ = BODY_PARSED;
      respondToClient();
      reportParseEvent(QStringLiteral("body"));
      reportParseEvent(QStringLiteral("resync"));
      reportParseEvent(QStringLiteral("dropped_bytes"), 1);
      return false;
    }
    sensorPackets.push_back(body.mid(offset, packetSize));
    offset += packetSize;
  }

  if (offset != bodySize || sensorPackets.size() != curHeader_.msg_count) {
    std::cerr << "[AndroidTcpClient] TCP body packet count failed: bodySize="
              << bodySize << " buffer_size=" << buffer_.size()
              << " msg_count=" << curHeader_.msg_count
              << " parsed_packets=" << sensorPackets.size()
              << " logical_offset=" << offset << std::endl;
    if (!bodyBuffer_.isEmpty()) {
      bodyBuffer_.remove(0, 1);
      buffer_.prepend(bodyBuffer_);
      bodyBuffer_.clear();
    }
    status_ = BODY_PARSED;
    respondToClient();
    reportParseEvent(QStringLiteral("body"));
    reportParseEvent(QStringLiteral("msg_count_mismatch"));
    reportParseEvent(QStringLiteral("resync"));
    reportParseEvent(QStringLiteral("dropped_bytes"), 1);
    return false;
  }

  bodyBuffer_.remove(0, bodySize);
  status_ = BODY_PARSED;
  lastSuccessTimestamp_ = curHeader_.timestamp_ns;
  reportParseEvent(QStringLiteral("tcp_message_parsed"));
  respondToClient();

  for (const auto &packet : sensorPackets) {
    if (callback_) {
      callback_(packet);
    }
  }
  return true;
}

void AndroidTcpClient::respondToClient() {
  if (socket_ && socket_->isWritable()) {
    socket_->write("ok");
    socket_->flush();
  }
}

void AndroidTcpClient::reportParseEvent(const QString &kind, uint64_t amount) {
  if (parseEventCallback_) {
    parseEventCallback_(kind, amount);
  }
}

AndroidSubnode::AndroidSubnode(const QString &name, const QString &subnodeHost,
                               int goalPort, int feedbackPort,
                               const QString &rootPath, int tcpPort,
                               QObject *parent)
    : BaseSubnode(name, subnodeHost, goalPort, feedbackPort, rootPath, parent),
      tcpPort_(tcpPort) {
  registerAndroidCommands();
  std::cout << "[" << name_.toStdString()
            << "] AndroidSubnode initialized (tcp_port=" << tcpPort_ << ")"
            << std::endl;
}

AndroidSubnode::~AndroidSubnode() {
  shutdownDevice();
  stopPacketWorker();
}

json AndroidSubnode::connect() {
  auto result = BaseSubnode::connect();
  if (!result.value("success", false)) {
    return result;
  }
  createPublisher(PORT_ANDROID_IMU, TOPIC_ANDROID_IMU, "json", 120.0, true);
  startPacketWorker();
  try {
    startTcpServer();
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
  return {{"success", true}, {"message", "Connected"}};
}

void AndroidSubnode::registerAndroidCommands() {
  registerCmd("start_device", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdStartDevice(goal, cmd, params);
  });
  registerCmd("restart_device", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdRestartDevice(goal, cmd, params);
  });
  registerCmd("stop_device", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdStopDevice(goal, cmd, params);
  });
  registerCmd("release_device", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdReleaseDevice(goal, cmd, params);
  });
  registerCmd("start_record", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdStartRecord(goal, cmd, params);
  });
  registerCmd("stop_record", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdStopRecord(goal, cmd, params);
  });
  registerCmd("set_fan", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdSetFan(goal, cmd, params);
  });
  registerCmd("restore_fan", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdRestoreFan(goal, cmd, params);
  });
  registerCmd("start_fan_cycle", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdStartFanCycle(goal, cmd, params);
  });
  registerCmd("stop_fan_cycle", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdStopFanCycle(goal, cmd, params);
  });
  registerCmd("start_gps_cycle", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdStartGpsCycle(goal, cmd, params);
  });
  registerCmd("stop_gps_cycle", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdStopGpsCycle(goal, cmd, params);
  });
  registerCmd("start_load", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdStartLoad(goal, cmd, params);
  });
  registerCmd("stop_load", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdStopLoad(goal, cmd, params);
  });
  registerCmd("check", [this](auto goal, const auto &cmd, const auto &params) {
    return cmdCheck(goal, cmd, params);
  });
}

void AndroidSubnode::setTcpPortFromParams(const json &params) {
  if (!params.is_object() || !params.contains("tcp_port")) {
    return;
  }
  try {
    tcpPort_ = params.at("tcp_port").get<int>();
  } catch (...) {
  }
}

void AndroidSubnode::startTcpServer() {
  if (QThread::currentThread() != thread()) {
    bool ok = false;
    QString error;
    QMetaObject::invokeMethod(
        this,
        [this, &ok, &error]() {
          try {
            startTcpServer();
            ok = true;
          } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
          }
        },
        Qt::BlockingQueuedConnection);
    if (!ok) {
      throw std::runtime_error(error.toStdString());
    }
    return;
  }

  if (tcpServer_ && tcpServer_->isListening()) {
    return;
  }
  tcpServer_ = std::make_unique<QTcpServer>();
  if (!tcpServer_->listen(QHostAddress::AnyIPv4, static_cast<quint16>(tcpPort_))) {
    const QString error = tcpServer_->errorString();
    tcpServer_.reset();
    throw std::runtime_error("Failed to listen on Android TCP port " +
                             std::to_string(tcpPort_) + ": " +
                             error.toStdString());
  }
  QObject::connect(tcpServer_.get(), &QTcpServer::newConnection, this,
                   &AndroidSubnode::onNewTcpConnection);
  std::cout << "[" << name_.toStdString()
            << "] Android TCP server listening on " << tcpPort_ << std::endl;
}

void AndroidSubnode::stopTcpServer() {
  if (QThread::currentThread() != thread()) {
    QMetaObject::invokeMethod(this, [this]() { stopTcpServer(); },
                              Qt::BlockingQueuedConnection);
    return;
  }

  const auto clients = tcpClients_;
  tcpClients_.clear();
  for (auto *client : clients) {
    if (client) {
      client->disconnect();
      client->deleteLater();
    }
  }

  if (tcpServer_) {
    tcpServer_->disconnect();
    tcpServer_->close();
    tcpServer_.reset();
  }
}

void AndroidSubnode::onNewTcpConnection() {
  while (tcpServer_ && tcpServer_->hasPendingConnections()) {
    QTcpSocket *socket = tcpServer_->nextPendingConnection();
    connectionIp_ = socket->peerAddress().toString();
    connectionProtocol_ = QStringLiteral("TCP");
    lastClientAddress_ =
        QStringLiteral("%1:%2").arg(connectionIp_).arg(socket->peerPort());
    tcpConnectionCount_++;
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    socket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, 256 * 1024);
    socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 512 * 1024);

    auto *client = new AndroidTcpClient(
        socket,
        [this](const QByteArray &data) { enqueueSensorDatagram(data); },
        [this](const QString &kind, uint64_t amount) {
          if (kind == QStringLiteral("header")) {
            parseHeaderFailureCount_.fetch_add(amount);
          } else if (kind == QStringLiteral("body")) {
            parseBodyFailureCount_.fetch_add(amount);
          } else if (kind == QStringLiteral("ready_read")) {
            readyReadCount_.fetch_add(amount);
          } else if (kind == QStringLiteral("read_bytes")) {
            readBytesCount_.fetch_add(amount);
          } else if (kind == QStringLiteral("tcp_buffer_size")) {
            uint64_t observed = maxTcpBufferSize_.load();
            while (amount > observed &&
                   !maxTcpBufferSize_.compare_exchange_weak(observed, amount)) {
            }
          } else if (kind == QStringLiteral("resync")) {
            resyncCount_.fetch_add(amount);
          } else if (kind == QStringLiteral("dropped_bytes")) {
            droppedBytesCount_.fetch_add(amount);
          } else if (kind == QStringLiteral("tcp_message_parsed")) {
            parsedTcpMessageCount_.fetch_add(amount);
          } else if (kind == QStringLiteral("msg_count_mismatch")) {
            msgCountMismatchCount_.fetch_add(amount);
          }
        },
        nullptr);
    QObject::connect(client, &QObject::destroyed, [this, client]() {
      auto it = std::find(tcpClients_.begin(), tcpClients_.end(), client);
      if (it != tcpClients_.end()) {
        tcpClients_.erase(it);
      }
    });
    tcpClients_.push_back(client);
    std::cout << "[" << name_.toStdString() << "] TCP client connected from "
              << connectionIp_.toStdString() << std::endl;
  }
}

void AndroidSubnode::enqueueSensorDatagram(const QByteArray &sensorData) {
  std::size_t depth = 0;
  {
    std::lock_guard<std::mutex> lock(packetQueueMutex_);
    packetQueue_.push_back(sensorData);
    depth = packetQueue_.size();
  }

  queuedPacketCount_++;
  uint64_t observed = maxPacketQueueDepth_.load();
  while (depth > observed &&
         !maxPacketQueueDepth_.compare_exchange_weak(
             observed, static_cast<uint64_t>(depth))) {
  }
  packetQueueCv_.notify_one();
}

void AndroidSubnode::startPacketWorker() {
  std::lock_guard<std::mutex> lock(packetQueueMutex_);
  if (packetWorkerRunning_) {
    return;
  }
  packetWorkerRunning_ = true;
  packetWorker_ = std::thread([this]() { packetWorkerLoop(); });
}

void AndroidSubnode::stopPacketWorker() {
  {
    std::lock_guard<std::mutex> lock(packetQueueMutex_);
    if (!packetWorkerRunning_ && !packetWorker_.joinable()) {
      return;
    }
    packetWorkerRunning_ = false;
  }
  packetQueueCv_.notify_all();
  if (packetWorker_.joinable()) {
    packetWorker_.join();
  }
}

void AndroidSubnode::packetWorkerLoop() {
  while (true) {
    QByteArray sensorData;
    {
      std::unique_lock<std::mutex> lock(packetQueueMutex_);
      packetQueueCv_.wait(lock, [this]() {
        return !packetQueue_.empty() || !packetWorkerRunning_;
      });
      if (packetQueue_.empty() && !packetWorkerRunning_) {
        break;
      }
      sensorData = std::move(packetQueue_.front());
      packetQueue_.pop_front();
      packetWorkerProcessing_ = true;
    }

    processSensorDatagram(sensorData);
    processedPacketCount_++;

    {
      std::lock_guard<std::mutex> lock(packetQueueMutex_);
      packetWorkerProcessing_ = false;
    }
    packetQueueCv_.notify_all();
  }
}

bool AndroidSubnode::waitForPacketQueueDrained(int timeoutMs) {
  std::unique_lock<std::mutex> lock(packetQueueMutex_);
  return packetQueueCv_.wait_for(
      lock, std::chrono::milliseconds(timeoutMs), [this]() {
        return packetQueue_.empty() && !packetWorkerProcessing_;
      });
}

void AndroidSubnode::processSensorDatagram(const QByteArray &sensorData) {
  sensorPacketCount_++;
  constexpr int headerSize = static_cast<int>(sizeof(AndroidNrealLinkMsgHeader));
  if (sensorData.size() <= headerSize) {
    return;
  }

  AndroidNrealLinkMsgHeader header{};
  std::memcpy(&header, sensorData.constData(), headerSize);
  if (header.payload_length + headerSize != sensorData.size()) {
    return;
  }
  if (static_cast<int>(header.magic) != kMobileGroupId ||
      header.msg_id != kMobileMsgId) {
    return;
  }
  mobilePacketCount_++;

  const int payloadStart = headerSize - 8;
  const int payloadLen = header.payload_length + 8;
  if (payloadStart < 0 || payloadStart + payloadLen > sensorData.size()) {
    return;
  }
  handleMobilePayload(sensorData.mid(payloadStart, payloadLen));
}

void AndroidSubnode::handleMobilePayload(const QByteArray &payload) {
  if (payload.size() < kMobilePayloadSize) {
    return;
  }

  const auto *raw = reinterpret_cast<const uint8_t *>(payload.constData());
  int offset = 0;
  const uint64_t tsUs = readLe<uint64_t>(raw, offset);
  const uint64_t onsensorTimestampNs = readLe<uint64_t>(raw, offset);
  const uint64_t timestampNs = readLe<uint64_t>(raw, offset);
  const uint32_t type = readLe<uint32_t>(raw, offset);

  std::array<float, 6> values{};
  for (float &value : values) {
    value = readLe<float>(raw, offset);
  }

  const double onsensorTimestampUs =
      static_cast<double>(onsensorTimestampNs) / 1000.0;
  const double freqTimestamp =
      timestampNs > 0 ? static_cast<double>(timestampNs) / 1e9
                      : static_cast<double>(tsUs) / 1e6;
  const double rawFrequencyHz = trackAndroidRawFrequency(type, freqTimestamp);
  const AndroidMobileRow row{tsUs, onsensorTimestampUs, timestampNs, type,
                             values};
  json message = {
      {"type", type},
      {"timestamp", tsUs},
      {"onsensor_timestamp_us", onsensorTimestampUs},
      {"timestamp_ns", timestampNs},
      {"frequency_hz", rawFrequencyHz},
      {"data", json::array({values[0], values[1], values[2],
                             values[3], values[4], values[5]})}};
  publish(PORT_ANDROID_IMU, message);
  publishCount_++;

  QMutexLocker locker(&recordStateLock_);
  if (!recordFlag_) {
    return;
  }
  writeCsvRow(row);
}

double AndroidSubnode::trackAndroidRawFrequency(uint32_t type,
                                                double timestampSec) {
  if (timestampSec <= 0.0) {
    return androidRawFrequenciesByType_[type];
  }

  auto &times = androidRawTimesByType_[type];
  if (!times.empty() && timestampSec < times.back() - 1.0) {
    times.clear();
    androidRawFrequenciesByType_[type] = 0.0;
  }
  if (!times.empty() && std::abs(timestampSec - times.back()) < 1e-9) {
    return androidRawFrequenciesByType_[type];
  }

  times.push_back(timestampSec);
  const double cutoff = timestampSec - 1.0;
  while (!times.empty() && times.front() < cutoff) {
    times.pop_front();
  }

  if (times.size() >= 3) {
    const int n = std::min(static_cast<int>(times.size()), 200);
    const double span = times.back() - times[times.size() - n];
    if (span > 0.001) {
      androidRawFrequenciesByType_[type] = (n - 1) / span;
    }
  }
  return androidRawFrequenciesByType_[type];
}

QString AndroidSubnode::runtimeDir() const {
  return QDir(projectRootPath()).filePath(QStringLiteral("resources/android_imu/arm64-v8a"));
}

QString AndroidSubnode::storageRootPath() const {
  return resolveStorageRootPath(rootPath_);
}

QString AndroidSubnode::generateMobileDataFilename() const {
  const QString timeText = QDateTime::currentDateTime().toString(QStringLiteral("yy_MM_dd_HH_mm_ss"));
  return QStringLiteral("%1_%2_%3_mobile_data.csv")
      .arg(timeText, connectionIp_, connectionProtocol_);
}

bool AndroidSubnode::openCsv(const QString &folderPath, const QString &filename) {
  QDir().mkpath(folderPath);
  recordCsvPath_ = QDir(folderPath).filePath(filename);
  csvFile_.setFileName(recordCsvPath_);
  if (!csvFile_.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    recordCsvPath_.clear();
    return false;
  }
  csvStream_.setDevice(&csvFile_);
  csvStream_ << "timestamp,onsensor_timestamp_us,timestamp_ns,type,data0,data1,data2,data3,data4,data5\n";
  csvStream_.flush();
  return true;
}

void AndroidSubnode::writeCsvRow(const json &row) {
  if (!csvFile_.isOpen()) {
    return;
  }
  csvStream_ << QString::number(row.value("timestamp", uint64_t(0))) << ','
             << QString::number(row.value("onsensor_timestamp_us", 0.0), 'f', 3) << ','
             << QString::number(row.value("timestamp_ns", uint64_t(0))) << ','
             << QString::number(row.value("type", uint32_t(0))) << ','
             << QString::number(row.value("data0", 0.0), 'g', 9) << ','
             << QString::number(row.value("data1", 0.0), 'g', 9) << ','
             << QString::number(row.value("data2", 0.0), 'g', 9) << ','
             << QString::number(row.value("data3", 0.0), 'g', 9) << ','
             << QString::number(row.value("data4", 0.0), 'g', 9) << ','
             << QString::number(row.value("data5", 0.0), 'g', 9) << '\n';
  csvWriteCount_++;
}

void AndroidSubnode::writeCsvRow(const AndroidMobileRow &row) {
  if (!csvFile_.isOpen()) {
    return;
  }
  csvStream_ << QString::number(row.timestamp) << ','
             << QString::number(row.onsensorTimestampUs, 'f', 3) << ','
             << QString::number(row.timestampNs) << ','
             << QString::number(row.type) << ','
             << QString::number(row.data[0], 'g', 9) << ','
             << QString::number(row.data[1], 'g', 9) << ','
             << QString::number(row.data[2], 'g', 9) << ','
             << QString::number(row.data[3], 'g', 9) << ','
             << QString::number(row.data[4], 'g', 9) << ','
             << QString::number(row.data[5], 'g', 9) << '\n';
  csvWriteCount_++;
}

QString AndroidSubnode::closeAndSortCsv() {
  const QString originalPath = recordCsvPath_;
  if (csvFile_.isOpen()) {
    csvStream_.flush();
    csvStream_.setDevice(nullptr);
    csvFile_.close();
  }
  recordCsvPath_.clear();
  if (originalPath.isEmpty() || !QFileInfo::exists(originalPath)) {
    return originalPath;
  }

  QFile input(originalPath);
  if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return originalPath;
  }
  QTextStream in(&input);
  const QString header = in.readLine();
  struct CsvRow {
    double sortKey = 0.0;
    QString line;
  };
  std::vector<CsvRow> rows;
  while (!in.atEnd()) {
    const QString line = in.readLine();
    if (!line.trimmed().isEmpty()) {
      const auto columns = line.split(',');
      bool ok = false;
      const double key = columns.size() > 1 ? columns.at(1).toDouble(&ok) : 0.0;
      rows.push_back({ok ? key : 0.0, line});
    }
  }
  input.close();

  std::stable_sort(rows.begin(), rows.end(),
                   [](const CsvRow &a, const CsvRow &b) {
                     return a.sortKey < b.sortKey;
                   });

  QFileInfo info(originalPath);
  QString sortedName = info.fileName();
  sortedName.replace(QStringLiteral("_TCP_"), QStringLiteral("_"));
  sortedName.replace(QStringLiteral("_UDP_"), QStringLiteral("_"));
  const QString sortedPath = info.dir().filePath(sortedName);
  const QString tmpPath = sortedPath + QStringLiteral(".tmp");

  QFile output(tmpPath);
  if (!output.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    return originalPath;
  }
  QTextStream out(&output);
  if (!header.isEmpty()) {
    out << header << '\n';
  }
  for (const auto &row : rows) {
    out << row.line << '\n';
  }
  out.flush();
  output.close();

  QFile::remove(sortedPath);
  if (!QFile::rename(tmpPath, sortedPath)) {
    QFile::remove(tmpPath);
    return originalPath;
  }
  if (sortedPath != originalPath) {
    QFile::remove(originalPath);
  }
  return sortedPath;
}

QString AndroidSubnode::runAdb(const QStringList &args, int timeoutMs,
                               bool allowFailure) {
  QProcess process;
  process.start(QStringLiteral("adb"), args);
  if (!process.waitForFinished(timeoutMs)) {
    process.kill();
    process.waitForFinished(1000);
    if (!allowFailure) {
      throw std::runtime_error(("adb " + args.join(' ') + " timeout").toStdString());
    }
  }
  const QString output = processOutput(process);
  if (process.exitCode() != 0 && !allowFailure) {
    throw std::runtime_error((output.isEmpty()
                                  ? QStringLiteral("adb %1 failed").arg(args.join(' '))
                                  : output)
                                 .toStdString());
  }
  return output;
}

void AndroidSubnode::stopLocalProcess(std::unique_ptr<QProcess> &process,
                                      const QString &label) {
  if (!process) {
    return;
  }
  if (process->state() != QProcess::NotRunning) {
    process->terminate();
    if (!process->waitForFinished(3000)) {
      process->kill();
      process->waitForFinished(1000);
    }
  }
  std::cout << "[" << name_.toStdString() << "] stopped "
            << label.toStdString() << std::endl;
  process.reset();
}

void AndroidSubnode::killRemoteGetImuData() {
  stopLocalProcess(adbImuProcess_, QStringLiteral("get_imu_data adb shell"));
  runAdb({"shell", "pkill -f get_imu_data"}, 5000, true);
}

void AndroidSubnode::setupAdbReverse() {
  runAdb({"reverse", QStringLiteral("tcp:%1").arg(tcpPort_),
          QStringLiteral("tcp:%1").arg(tcpPort_)},
         10000, false);
}

void AndroidSubnode::pushAndroidRuntimeFiles() {
  const QString dir = runtimeDir();
  const QStringList files = {QStringLiteral("libc++_shared.so"),
                             QStringLiteral("get_imu_data")};
  for (const auto &file : files) {
    const QString path = QDir(dir).filePath(file);
    if (!QFileInfo::exists(path)) {
      throw std::runtime_error(("Missing Android runtime file: " + path).toStdString());
    }
    runAdb({"push", path, QStringLiteral("/data/local/tmp/")}, 30000, false);
  }
  runAdb({"shell", "chmod 755 /data/local/tmp/get_imu_data"}, 10000, false);
}

void AndroidSubnode::startLongAdbShell(std::unique_ptr<QProcess> &process,
                                       const QString &shellCommand,
                                       const QString &label) {
  stopLocalProcess(process, label);
  process = std::make_unique<QProcess>();
  process->setStandardOutputFile(QProcess::nullDevice());
  process->setStandardErrorFile(QProcess::nullDevice());
  process->start(QStringLiteral("adb"), {QStringLiteral("shell"), shellCommand});
  if (!process->waitForStarted(5000)) {
    process.reset();
    throw std::runtime_error(("failed to start " + label).toStdString());
  }
  QThread::msleep(200);
  if (process->state() == QProcess::NotRunning) {
    const QString output = processOutput(*process);
    process.reset();
    throw std::runtime_error((label + " exited immediately: " + output).toStdString());
  }
}

void AndroidSubnode::startRemoteGetImuData() {
  startLongAdbShell(
      adbImuProcess_,
      QStringLiteral("export LD_LIBRARY_PATH=/data/local/tmp && /data/local/tmp/get_imu_data 127.0.0.1"),
      QStringLiteral("get_imu_data"));
}

void AndroidSubnode::setFanSpeed(int speed) {
  if (speed < 0 || speed > 100) {
    throw std::runtime_error("fan speed must be between 0 and 100");
  }
  runAdb({"root"}, 10000, true);
  runAdb({"shell", "echo 1 > /sys/class/fan/max31760/speed_fixed_flag"}, 10000, false);
  runAdb({"shell", QStringLiteral("echo %1 > /sys/class/fan/max31760/speed_control").arg(speed)},
         10000, false);
}

void AndroidSubnode::restoreFanPolicy() {
  runAdb({"root"}, 10000, true);
  runAdb({"shell", "echo 0 > /sys/class/fan/max31760/speed_control"}, 10000, false);
  runAdb({"shell", "echo 0 > /sys/class/fan/max31760/speed_fixed_flag"}, 10000, false);
}

void AndroidSubnode::installRemoteScript(const QString &remotePath,
                                         const QString &script) {
  runAdb({"root"}, 10000, true);
  QProcess process;
  process.start(QStringLiteral("adb"),
                {QStringLiteral("shell"),
                 QStringLiteral("cat > %1").arg(remotePath)});
  if (!process.waitForStarted(5000)) {
    throw std::runtime_error("failed to start adb shell cat");
  }
  process.write(script.toUtf8());
  process.closeWriteChannel();
  if (!process.waitForFinished(10000) || process.exitCode() != 0) {
    const QString output = processOutput(process);
    throw std::runtime_error(("install remote script failed: " + output).toStdString());
  }
  runAdb({"shell", QStringLiteral("chmod 755 %1").arg(remotePath)}, 10000, false);
}

void AndroidSubnode::startFanCycle() {
  stopFanCycle();
  installRemoteScript(QStringLiteral("/data/local/tmp/recordlab_fan_cycle.sh"),
                      fanCycleScript());
  startLongAdbShell(adbFanCycleProcess_,
                    QStringLiteral("sh /data/local/tmp/recordlab_fan_cycle.sh"),
                    QStringLiteral("fan cycle"));
}

void AndroidSubnode::stopFanCycle() {
  stopLocalProcess(adbFanCycleProcess_, QStringLiteral("fan cycle"));
  runAdb({"shell", "pkill -f recordlab_fan_cycle.sh"}, 10000, true);
  restoreFanPolicy();
}

void AndroidSubnode::startGpsCycle() {
  stopGpsCycle();
  installRemoteScript(QStringLiteral("/data/local/tmp/recordlab_gps_cycle.sh"),
                      gpsCycleScript());
  startLongAdbShell(adbGpsCycleProcess_,
                    QStringLiteral("sh /data/local/tmp/recordlab_gps_cycle.sh"),
                    QStringLiteral("gps cycle"));
}

void AndroidSubnode::stopGpsCycle() {
  stopLocalProcess(adbGpsCycleProcess_, QStringLiteral("gps cycle"));
  runAdb({"shell", "pkill -f recordlab_gps_cycle.sh"}, 10000, true);
  runAdb({"shell", "cmd location set-location-enabled false"}, 10000, true);
  runAdb({"shell", "settings put secure location_providers_allowed -gps"}, 10000, true);
}

void AndroidSubnode::startLoad(const QString &mode) {
  const QString normalized = mode.trimmed().toLower();
  if (normalized != QStringLiteral("high") && normalized != QStringLiteral("wave")) {
    throw std::runtime_error("load mode must be high or wave");
  }
  stopLoad();
  installRemoteScript(QStringLiteral("/data/local/tmp/recordlab_load.sh"),
                      loadScript());
  startLongAdbShell(adbLoadProcess_,
                    QStringLiteral("sh /data/local/tmp/recordlab_load.sh %1").arg(normalized),
                    QStringLiteral("load"));
}

void AndroidSubnode::stopLoad() {
  stopLocalProcess(adbLoadProcess_, QStringLiteral("load"));
  runAdb({"shell", "pkill -f recordlab_load.sh"}, 10000, true);
  runAdb({"shell", "pkill -f recordlab_cpu_load"}, 10000, true);
  runAdb({"shell", "pkill -f recordlab_mem_load"}, 10000, true);
  runAdb({"shell", "pkill -f recordlab_io_load"}, 10000, true);
  runAdb({"shell", "rm -f /data/local/tmp/recordlab_stress_*.img"}, 10000, true);
}

void AndroidSubnode::shutdownDevice() {
  {
    QMutexLocker locker(&recordStateLock_);
    if (recordFlag_) {
      recordFlag_ = false;
      closeAndSortCsv();
    } else if (csvFile_.isOpen()) {
      closeAndSortCsv();
    }
  }
  stopTcpServer();
  try { stopGpsCycle(); } catch (...) {}
  try { stopFanCycle(); } catch (...) {}
  try { stopLoad(); } catch (...) {}
  try { killRemoteGetImuData(); } catch (...) {}
}

json AndroidSubnode::cmdStartDevice(uint32_t, const std::string &, const json &params) {
  try {
    setTcpPortFromParams(params);
    startTcpServer();
    return {{"success", true}, {"message", "TCP server started"}};
  } catch (const std::exception &e) {
    lastError_ = QString::fromUtf8(e.what());
    return {{"success", false}, {"message", e.what()}};
  }
}

json AndroidSubnode::cmdRestartDevice(uint32_t, const std::string &, const json &params) {
  try {
    setTcpPortFromParams(params);
    {
      QMutexLocker locker(&recordStateLock_);
      if (recordFlag_) {
        recordFlag_ = false;
        closeAndSortCsv();
      }
    }
    stopTcpServer();
    stopGpsCycle();
    stopFanCycle();
    stopLoad();
    killRemoteGetImuData();
    setupAdbReverse();
    pushAndroidRuntimeFiles();
    startTcpServer();
    startRemoteGetImuData();
    return {{"success", true},
            {"message", "Android device restarted on TCP:" + std::to_string(tcpPort_)}};
  } catch (const std::exception &e) {
    lastError_ = QString::fromUtf8(e.what());
    return {{"success", false}, {"message", e.what()}};
  }
}

json AndroidSubnode::cmdStopDevice(uint32_t, const std::string &, const json &) {
  stopTcpServer();
  try { stopGpsCycle(); } catch (const std::exception &e) { std::cerr << e.what() << std::endl; }
  try { stopFanCycle(); } catch (const std::exception &e) { std::cerr << e.what() << std::endl; }
  try { stopLoad(); } catch (const std::exception &e) { std::cerr << e.what() << std::endl; }
  try { killRemoteGetImuData(); } catch (const std::exception &e) { std::cerr << e.what() << std::endl; }
  return {{"success", true}, {"message", "TCP server stopped"}};
}

json AndroidSubnode::cmdReleaseDevice(uint32_t, const std::string &, const json &) {
  shutdownDevice();
  return {{"success", true}, {"message", "Released"}};
}

json AndroidSubnode::cmdStartRecord(uint32_t, const std::string &, const json &params) {
  const QString datasetName = QString::fromStdString(params.value("dataset_name", std::string()));
  if (datasetName.trimmed().isEmpty()) {
    return {{"success", false}, {"message", "Missing: dataset_name"}};
  }
  QMutexLocker locker(&recordStateLock_);
  if (recordFlag_) {
    return {{"success", false}, {"message", "Already recording"}};
  }
  const QString recordPath = QDir(storageRootPath()).filePath(datasetName);
  if (!openCsv(recordPath, generateMobileDataFilename())) {
    return {{"success", false}, {"message", "Failed to open CSV"}};
  }
  recordFlag_ = true;
  return {{"success", true},
          {"message", "Recording started: " + datasetName.toStdString()}};
}

json AndroidSubnode::cmdStopRecord(uint32_t, const std::string &, const json &) {
  {
    QMutexLocker locker(&recordStateLock_);
    if (!recordFlag_) {
      return {{"success", false}, {"message", "Not recording"}};
    }
  }

  const bool drained = waitForPacketQueueDrained(3000);
  if (!drained) {
    std::cerr << "[" << name_.toStdString()
              << "] stop_record: packet queue did not fully drain before CSV close"
              << std::endl;
  }

  QMutexLocker locker(&recordStateLock_);
  recordFlag_ = false;
  const QString csvPath = closeAndSortCsv();
  return {{"success", true},
          {"message", "Recording stopped"},
          {"csv_path", csvPath.toStdString()}};
}

json AndroidSubnode::cmdSetFan(uint32_t, const std::string &, const json &params) {
  try {
    const int speed = params.value("speed", 0);
    setFanSpeed(speed);
    return {{"success", true},
            {"message", "Fan speed fixed to " + std::to_string(speed)}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json AndroidSubnode::cmdRestoreFan(uint32_t, const std::string &, const json &) {
  try {
    restoreFanPolicy();
    return {{"success", true}, {"message", "Fan auto policy restored"}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json AndroidSubnode::cmdStartFanCycle(uint32_t, const std::string &, const json &) {
  try {
    startFanCycle();
    return {{"success", true}, {"message", "Fan cycle started: 0/100 every 60 seconds"}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json AndroidSubnode::cmdStopFanCycle(uint32_t, const std::string &, const json &) {
  try {
    stopFanCycle();
    return {{"success", true}, {"message", "Fan cycle stopped"}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json AndroidSubnode::cmdStartGpsCycle(uint32_t, const std::string &, const json &) {
  try {
    startGpsCycle();
    return {{"success", true}, {"message", "GPS cycle started: on/off every 60 seconds"}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json AndroidSubnode::cmdStopGpsCycle(uint32_t, const std::string &, const json &) {
  try {
    stopGpsCycle();
    return {{"success", true}, {"message", "GPS cycle stopped"}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json AndroidSubnode::cmdStartLoad(uint32_t, const std::string &, const json &params) {
  try {
    const QString mode = QString::fromStdString(params.value("mode", std::string("high")));
    startLoad(mode);
    return {{"success", true},
            {"message", "Android load started: " + mode.trimmed().toLower().toStdString()}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json AndroidSubnode::cmdStopLoad(uint32_t, const std::string &, const json &) {
  try {
    stopLoad();
    return {{"success", true}, {"message", "Android load stopped"}};
  } catch (const std::exception &e) {
    return {{"success", false}, {"message", e.what()}};
  }
}

json AndroidSubnode::cmdCheck(uint32_t, const std::string &, const json &) {
  return onCheck();
}

json AndroidSubnode::onCheck() {
  const bool adbImuRunning =
      adbImuProcess_ && adbImuProcess_->state() != QProcess::NotRunning;
  std::size_t queueDepth = 0;
  bool workerProcessing = false;
  {
    std::lock_guard<std::mutex> lock(packetQueueMutex_);
    queueDepth = packetQueue_.size();
    workerProcessing = packetWorkerProcessing_;
  }
  return {{"success", true},
          {"message", tcpServer_ && tcpServer_->isListening()
                          ? "TCP server running"
                          : "TCP server not started"},
          {"tcp_port", tcpPort_},
          {"tcp_server_running", tcpServer_ && tcpServer_->isListening()},
          {"adb_get_imu_data_running", adbImuRunning},
          {"tcp_connections", tcpConnectionCount_.load()},
          {"sensor_packets", sensorPacketCount_.load()},
          {"mobile_packets", mobilePacketCount_.load()},
          {"queued_packets", queuedPacketCount_.load()},
          {"processed_packets", processedPacketCount_.load()},
          {"packet_queue_depth", queueDepth},
          {"packet_worker_processing", workerProcessing},
          {"max_packet_queue_depth", maxPacketQueueDepth_.load()},
          {"parse_header_failures", parseHeaderFailureCount_.load()},
          {"parse_body_failures", parseBodyFailureCount_.load()},
          {"ready_read_calls", readyReadCount_.load()},
          {"read_bytes", readBytesCount_.load()},
          {"max_tcp_buffer_size", maxTcpBufferSize_.load()},
          {"resync_count", resyncCount_.load()},
          {"dropped_bytes", droppedBytesCount_.load()},
          {"parsed_tcp_messages", parsedTcpMessageCount_.load()},
          {"msg_count_mismatches", msgCountMismatchCount_.load()},
          {"csv_rows_written", csvWriteCount_.load()},
          {"published_packets", publishCount_.load()},
          {"last_client", lastClientAddress_.toStdString()},
          {"last_error", lastError_.toStdString()}};
}

json AndroidSubnode::onRelease() {
  shutdownDevice();
  return {{"success", true}, {"message", ""}};
}

json AndroidSubnode::onEstop() {
  shutdownDevice();
  return {{"success", true}, {"message", ""}};
}

int androidSubnodeMain(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);

  auto signalHandler = [](int signum) {
    std::cout << "Received signal " << signum << ", shutting down..."
              << std::endl;
    QCoreApplication::quit();
  };
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  QCommandLineParser parser;
  parser.addOption({"name", "Node name", "android"});
  parser.addOption({"host", "Host address", "127.0.0.1"});
  parser.addOption({"goal-port", "Goal port", "5698"});
  parser.addOption({"feedback-port", "Feedback port", "5699"});
  parser.addOption({"root-path", "Data root path", "./data"});
  parser.addOption({"tcp-port", "Android TCP port", QString::number(kAndroidTcpPort)});
  parser.process(app);

  QString name = parser.value("name");
  if (name.isEmpty()) {
    name = QStringLiteral("android");
  }
  QString host = parser.value("host");
  if (host.isEmpty()) {
    host = QStringLiteral("127.0.0.1");
  }
  int goalPort = parser.value("goal-port").toInt();
  if (goalPort == 0) {
    goalPort = 5698;
  }
  int feedbackPort = parser.value("feedback-port").toInt();
  if (feedbackPort == 0) {
    feedbackPort = 5699;
  }
  QString rootPath = parser.value("root-path");
  if (rootPath.isEmpty()) {
    rootPath = QStringLiteral("./data");
  }
  int tcpPort = parser.value("tcp-port").toInt();
  if (tcpPort == 0) {
    tcpPort = kAndroidTcpPort;
  }

  QTimer ticker;
  QObject::connect(&ticker, &QTimer::timeout, []() {});
  ticker.start(500);

  AndroidSubnode subnode(name, host, goalPort, feedbackPort, rootPath, tcpPort);
  const auto result = subnode.connect();
  if (!result.value("success", false)) {
    std::cerr << "Failed to connect: "
              << result.value("message", std::string("Unknown error"))
              << std::endl;
    return 1;
  }

  std::cout << "[" << name.toStdString() << "] AndroidSubnode spinning..."
            << std::endl;
  return app.exec();
}

} // namespace recordlab::subnodes
