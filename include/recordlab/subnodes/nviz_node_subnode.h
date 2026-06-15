/*
 * NvizNodeSubnode - NViz 节点子节点
 *
 * 继承 MainSubnode，添加 NViz 设备特有功能：
 * - XRLinkDevice: UDP/TCP 数据接收
 * - PayloadParser: 根据 plot.json 解析二进制负载
 *
 */
#pragma once

#include "recordlab/subnodes/main_subnode.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>

#include <memory>
#include <optional>
#include <deque>
#include <unordered_map>
#include <vector>

namespace recordlab::subnodes {

// ============================================================================
// NRealLink 二进制协议结构
// ============================================================================

#pragma pack(push, 1)
struct NrealLinkMsgHeader {
  uint8_t magic;
  int32_t msg_id;
  int32_t payload_length;
  uint64_t time_stamp;
};
#pragma pack(pop)

static_assert(sizeof(NrealLinkMsgHeader) == 17,
              "NrealLinkMsgHeader must be 17 bytes");

constexpr uint8_t NREAL_LINK_MAGIC_ID = 0xFD;

// ============================================================================
// TCP 外层协议头（30 bytes）
// ============================================================================

#pragma pack(push, 1)
struct TcpMsgHeader {
  uint8_t version;
  uint8_t magic_num;       // 期望 239
  uint8_t serialize_method;
  uint8_t service_num;
  uint8_t msg_type;
  uint32_t msg_count;
  uint32_t length;
  uint64_t timestamp_ns;
  uint64_t packet_id;
  uint8_t crc;
};
#pragma pack(pop)

static_assert(sizeof(TcpMsgHeader) == 30, "TcpMsgHeader must be 30 bytes");

// ============================================================================
// PayloadParser — 根据 plot.json 解析二进制 IMU payload
// ============================================================================

class PayloadParser {
public:
  struct ParsedField {
    std::string name;
    std::string type;
    double value = 0.0;
  };

  explicit PayloadParser(const QString &plotJsonPath);
  PayloadParser() = default;

  bool isLoaded() const { return loaded_; }
  bool hasTemplate(int groupId, int msgId) const;
  std::string messageName(int groupId, int msgId) const;

  std::vector<double> parse(int groupId, int msgId,
                            const QByteArray &payload) const;
  std::vector<ParsedField> parseFields(int groupId, int msgId,
                                       const QByteArray &payload) const;

private:
  struct FieldSpec {
    int byteSize;
    bool isSigned;
    bool isFloat;
    int count;
    std::string name;
    std::string type;
    bool hidden;
  };

  struct MessageTemplate {
    std::string messageName;
    std::vector<FieldSpec> fields;
  };

  std::unordered_map<int64_t, MessageTemplate> templates_;
  bool loaded_ = false;

  static int64_t makeKey(int groupId, int msgId) {
    return (static_cast<int64_t>(groupId) << 32) | static_cast<int64_t>(msgId);
  }

  static FieldSpec parseTypeString(const std::string &typeStr, int arraySize);
};

// ============================================================================
// NrealLinkTcpClient — 单个 TCP 客户端连接的解析状态机
// ============================================================================

class NrealLinkTcpClient : public QObject {
  Q_OBJECT
public:
  using DataCallback =
      std::function<void(const QByteArray &sensorData)>;

  explicit NrealLinkTcpClient(QTcpSocket *socket, DataCallback cb,
                               QObject *parent = nullptr);
  ~NrealLinkTcpClient() override;

private slots:
  void onReadyRead();
  void onDisconnected();

private:
  enum ParserStatus { INIT = 0, HEAD_PARSED = 1, BODY_PARSED = 2 };

  bool parseHead();
  bool parseBody();
  void responseToClient();
  static uint8_t crc8(const uint8_t *data, int size);

  QTcpSocket *socket_;
  DataCallback callback_;

  QByteArray buffer_;
  TcpMsgHeader curHeader_{};
  uint64_t lastSuccessTimestamp_ = 0;
  ParserStatus status_ = INIT;
  bool handlingReadyRead_ = false;
  bool readyReadAgain_ = false;

  // 下一个 recv 的 submsg_type 期望
  // TCP 协议每个 recv 前面有 1 字节 TcpPacketMsgHeader
  bool expectingHeaderPacket_ = true;

  static constexpr uint8_t TCP_MAGIC_NUM = 239;
  static constexpr uint32_t TCP_MAX_FREQ_COUNT = 50000;
};

// ============================================================================
// XRLinkDevice — XRLink UDP/TCP 设备
// ============================================================================

class XRLinkDevice : public BaseDevice {
public:
  using JsonCallback = std::function<void(const nlohmann::json &)>;

  explicit XRLinkDevice(const std::string &host = "127.0.0.1",
                        int udpPort = 7099, int tcpPort = 8099,
                        bool enableUdp = true, bool enableTcp = false);
  ~XRLinkDevice() override;

  nlohmann::json initialize(const nlohmann::json &params) override;
  nlohmann::json start(const nlohmann::json &params = {}) override;
  nlohmann::json stop(const nlohmann::json &params = {}) override;
  nlohmann::json release() override;
  nlohmann::json check() override;
  void setImuDataCallback(ImuCallback cb) override {
    imuCallback_ = std::move(cb);
  }
  void setImageDataCallback(ImageCallback cb) override {
    imageCallback_ = std::move(cb);
  }
  void setTimeDelayCallback(JsonCallback cb) { timeDelayCallback_ = std::move(cb); }
  void setTreeDataCallback(JsonCallback cb) { treeDataCallback_ = std::move(cb); }

private:
  void onUdpData();
  void onNewTcpConnection();
  void processSensorDatagram(const QByteArray &sensorData);
  void dispatchParsedPayload(int groupId, int msgId,
                             const QByteArray &payload);
  std::optional<nlohmann::json> buildTimeDelayMessage(
      const std::vector<double> &parsed) const;
  void maybePublishTreeData(int groupId, int msgId, const QByteArray &payload);

  std::string host_;
  int udpPort_;
  int tcpPort_;
  bool enableUdp_;
  bool enableTcp_;

  std::unique_ptr<QUdpSocket> udpSocket_;
  std::unique_ptr<QTcpServer> tcpServer_;
  std::vector<NrealLinkTcpClient *> tcpClients_;

  ImuCallback imuCallback_;
  ImageCallback imageCallback_;
  JsonCallback timeDelayCallback_;
  JsonCallback treeDataCallback_;

  bool running_ = false;

  PayloadParser payloadParser_;
  uint64_t packetCount_ = 0;
  std::unordered_map<int64_t, int64_t> lastTreePublishNs_;
  std::unordered_map<int64_t, std::deque<int64_t>> treeArrivalNs_;
};

// ============================================================================
// NvizNodeSubnode
// ============================================================================

class NvizNodeSubnode : public MainSubnode {
  Q_OBJECT

public:
  explicit NvizNodeSubnode(const QString &name = "glasses_nviz_node",
                           const QString &subnodeHost = "127.0.0.1",
                           int goalPort = 5694, int feedbackPort = 5695,
                           const QString &rootPath = "./output",
                           const QString &deviceHost = "127.0.0.1",
                           int udpPort = 7099, int tcpPort = 8099,
                           bool enableUdp = true, bool enableTcp = false,
                           QObject *parent = nullptr);
  ~NvizNodeSubnode() override;

private:
  std::unique_ptr<XRLinkDevice> xrLinkDevice_;
};

// 独立可执行文件入口函数。
int nvizNodeSubnodeMain(int argc, char *argv[]);

} // namespace recordlab::subnodes
