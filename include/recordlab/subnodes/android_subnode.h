#pragma once

#include "recordlab/subnodes/base_subnode.h"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace recordlab::subnodes {

#pragma pack(push, 1)
struct AndroidNrealLinkMsgHeader {
  uint8_t magic;
  int32_t msg_id;
  int32_t payload_length;
  uint64_t time_stamp;
};

struct AndroidTcpMsgHeader {
  uint8_t version;
  uint8_t magic_num;
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

static_assert(sizeof(AndroidNrealLinkMsgHeader) == 17,
              "AndroidNrealLinkMsgHeader must be 17 bytes");
static_assert(sizeof(AndroidTcpMsgHeader) == 30,
              "AndroidTcpMsgHeader must be 30 bytes");

struct AndroidMobileRow {
  uint64_t timestamp = 0;
  double onsensorTimestampUs = 0.0;
  uint64_t timestampNs = 0;
  uint32_t type = 0;
  std::array<float, 6> data{};
};

class AndroidTcpClient : public QObject {
  Q_OBJECT

public:
  using DataCallback = std::function<void(const QByteArray &)>;
  using ParseEventCallback = std::function<void(const QString &, uint64_t)>;

  explicit AndroidTcpClient(QTcpSocket *socket, DataCallback callback,
                            ParseEventCallback parseEventCallback = {},
                            QObject *parent = nullptr);
  ~AndroidTcpClient() override;

private slots:
  void onReadyRead();
  void onDisconnected();

private:
  enum ParserStatus { INIT = 0, HEAD_PARSED = 1, BODY_PARSED = 2 };

  void parseStream();
  bool parseHead();
  bool parseBody();
  void respondToClient();
  void reportParseEvent(const QString &kind, uint64_t amount = 1);
  static uint8_t crc8(const uint8_t *data, int size);

  QTcpSocket *socket_ = nullptr;
  DataCallback callback_;
  ParseEventCallback parseEventCallback_;
  QByteArray buffer_;
  QByteArray bodyBuffer_;
  AndroidTcpMsgHeader curHeader_{};
  uint64_t lastSuccessTimestamp_ = 0;
  ParserStatus status_ = INIT;
  bool handlingReadyRead_ = false;
  bool readyReadAgain_ = false;
};

class AndroidSubnode : public BaseSubnode {
  Q_OBJECT

public:
  explicit AndroidSubnode(const QString &name = QStringLiteral("android"),
                          const QString &subnodeHost = QStringLiteral("127.0.0.1"),
                          int goalPort = 5698, int feedbackPort = 5699,
                          const QString &rootPath = QStringLiteral("./data"),
                          int tcpPort = 8100, QObject *parent = nullptr);
  ~AndroidSubnode() override;

  nlohmann::json connect();
  nlohmann::json onRelease() override;
  nlohmann::json onEstop() override;
  nlohmann::json onCheck() override;

private:
  nlohmann::json cmdStartDevice(uint32_t goalId, const std::string &cmd,
                                const nlohmann::json &params);
  nlohmann::json cmdRestartDevice(uint32_t goalId, const std::string &cmd,
                                  const nlohmann::json &params);
  nlohmann::json cmdStopDevice(uint32_t goalId, const std::string &cmd,
                               const nlohmann::json &params);
  nlohmann::json cmdReleaseDevice(uint32_t goalId, const std::string &cmd,
                                  const nlohmann::json &params);
  nlohmann::json cmdStartRecord(uint32_t goalId, const std::string &cmd,
                                const nlohmann::json &params);
  nlohmann::json cmdStopRecord(uint32_t goalId, const std::string &cmd,
                               const nlohmann::json &params);
  nlohmann::json cmdSetFan(uint32_t goalId, const std::string &cmd,
                           const nlohmann::json &params);
  nlohmann::json cmdRestoreFan(uint32_t goalId, const std::string &cmd,
                               const nlohmann::json &params);
  nlohmann::json cmdStartFanCycle(uint32_t goalId, const std::string &cmd,
                                  const nlohmann::json &params);
  nlohmann::json cmdStopFanCycle(uint32_t goalId, const std::string &cmd,
                                 const nlohmann::json &params);
  nlohmann::json cmdStartGpsCycle(uint32_t goalId, const std::string &cmd,
                                  const nlohmann::json &params);
  nlohmann::json cmdStopGpsCycle(uint32_t goalId, const std::string &cmd,
                                 const nlohmann::json &params);
  nlohmann::json cmdStartLoad(uint32_t goalId, const std::string &cmd,
                              const nlohmann::json &params);
  nlohmann::json cmdStopLoad(uint32_t goalId, const std::string &cmd,
                             const nlohmann::json &params);
  nlohmann::json cmdCheck(uint32_t goalId, const std::string &cmd,
                          const nlohmann::json &params);

  void registerAndroidCommands();
  void setTcpPortFromParams(const nlohmann::json &params);
  void startTcpServer();
  void stopTcpServer();
  void onNewTcpConnection();
  void enqueueSensorDatagram(const QByteArray &sensorData);
  void startPacketWorker();
  void stopPacketWorker();
  void packetWorkerLoop();
  bool waitForPacketQueueDrained(int timeoutMs);
  void processSensorDatagram(const QByteArray &sensorData);
  void handleMobilePayload(const QByteArray &payload);
  double trackAndroidRawFrequency(uint32_t type, double timestampSec);

  void shutdownDevice();
  void stopLocalProcess(std::unique_ptr<QProcess> &process,
                        const QString &label);
  void killRemoteGetImuData();
  void setupAdbReverse();
  void pushAndroidRuntimeFiles();
  void startRemoteGetImuData();

  QString runtimeDir() const;
  QString storageRootPath() const;
  QString generateMobileDataFilename() const;
  bool openCsv(const QString &folderPath, const QString &filename);
  QString closeAndSortCsv();
  void writeCsvRow(const nlohmann::json &row);
  void writeCsvRow(const AndroidMobileRow &row);

  QString runAdb(const QStringList &args, int timeoutMs = 10000,
                 bool allowFailure = false);
  void setFanSpeed(int speed);
  void restoreFanPolicy();
  void installRemoteScript(const QString &remotePath, const QString &script);
  void startLongAdbShell(std::unique_ptr<QProcess> &process,
                         const QString &shellCommand,
                         const QString &label);
  void startFanCycle();
  void stopFanCycle();
  void startGpsCycle();
  void stopGpsCycle();
  void startLoad(const QString &mode);
  void stopLoad();

  int tcpPort_ = 8100;
  std::unique_ptr<QTcpServer> tcpServer_;
  std::vector<AndroidTcpClient *> tcpClients_;
  QString connectionIp_ = QStringLiteral("unknown");
  QString connectionProtocol_ = QStringLiteral("TCP");

  QMutex recordStateLock_;
  bool recordFlag_ = false;
  QString recordCsvPath_;
  QFile csvFile_;
  QTextStream csvStream_;

  std::mutex packetQueueMutex_;
  std::condition_variable packetQueueCv_;
  std::deque<QByteArray> packetQueue_;
  std::thread packetWorker_;
  bool packetWorkerRunning_ = false;
  bool packetWorkerProcessing_ = false;
  std::unordered_map<uint32_t, std::deque<double>> androidRawTimesByType_;
  std::unordered_map<uint32_t, double> androidRawFrequenciesByType_;

  std::atomic<uint64_t> tcpConnectionCount_{0};
  std::atomic<uint64_t> sensorPacketCount_{0};
  std::atomic<uint64_t> mobilePacketCount_{0};
  std::atomic<uint64_t> queuedPacketCount_{0};
  std::atomic<uint64_t> processedPacketCount_{0};
  std::atomic<uint64_t> csvWriteCount_{0};
  std::atomic<uint64_t> publishCount_{0};
  std::atomic<uint64_t> parseHeaderFailureCount_{0};
  std::atomic<uint64_t> parseBodyFailureCount_{0};
  std::atomic<uint64_t> readyReadCount_{0};
  std::atomic<uint64_t> readBytesCount_{0};
  std::atomic<uint64_t> maxTcpBufferSize_{0};
  std::atomic<uint64_t> resyncCount_{0};
  std::atomic<uint64_t> droppedBytesCount_{0};
  std::atomic<uint64_t> parsedTcpMessageCount_{0};
  std::atomic<uint64_t> msgCountMismatchCount_{0};
  std::atomic<uint64_t> maxPacketQueueDepth_{0};
  QString lastClientAddress_;
  QString lastError_;

  std::unique_ptr<QProcess> adbImuProcess_;
  std::unique_ptr<QProcess> adbLoadProcess_;
  std::unique_ptr<QProcess> adbFanCycleProcess_;
  std::unique_ptr<QProcess> adbGpsCycleProcess_;
};

int androidSubnodeMain(int argc, char *argv[]);

} // namespace recordlab::subnodes
