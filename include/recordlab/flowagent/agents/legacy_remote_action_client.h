#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

namespace zmq {
class context_t;
class socket_t;
} // namespace zmq

namespace recordlab::flowagent::agents {

class LegacyRemoteActionClient {
public:
  using FeedbackCallback =
      std::function<void(uint32_t goalId, const nlohmann::json &feedback)>;
  using ResultCallback = std::function<void(uint32_t goalId,
                                            const nlohmann::json &result,
                                            bool success)>;

  LegacyRemoteActionClient(std::string host, int goalPort, int feedbackPort,
                           int requestTimeoutMs = 5000);
  ~LegacyRemoteActionClient();

  bool waitForServer(int timeoutMs, std::string *errorMessage = nullptr);
  uint32_t sendGoal(const nlohmann::json &goal,
                    FeedbackCallback feedbackCallback = nullptr,
                    ResultCallback resultCallback = nullptr);
  bool waitForResult(uint32_t goalId, int timeoutMs = 0);

private:
  struct PendingGoal {
    std::string remoteGoalId;
    FeedbackCallback feedbackCallback;
    ResultCallback resultCallback;
    nlohmann::json result;
    bool success = false;
    bool resultReceived = false;
  };

  std::string makeRemoteGoalId(uint32_t localGoalId) const;
  nlohmann::json callGoalSocket(const nlohmann::json &request);
  void resetGoalSocket();
  void listenLoop();
  void handleFeedback(const nlohmann::json &message);
  void handleResult(const nlohmann::json &message);
  void removeGoal(uint32_t goalId);

  std::string host_;
  int goalPort_ = 0;
  int feedbackPort_ = 0;
  int requestTimeoutMs_ = 5000;

  std::unique_ptr<zmq::context_t> context_;
  std::unique_ptr<zmq::socket_t> goalSocket_;
  std::unique_ptr<zmq::socket_t> feedbackSocket_;

  std::atomic<bool> running_{false};
  std::thread listenerThread_;
  std::atomic<uint32_t> nextGoalId_{1};

  mutable std::mutex socketMutex_;
  std::mutex pendingMutex_;
  std::condition_variable pendingCv_;
  std::map<uint32_t, PendingGoal> pendingByLocalId_;
  std::map<std::string, uint32_t> localIdByRemoteId_;
};

} // namespace recordlab::flowagent::agents
