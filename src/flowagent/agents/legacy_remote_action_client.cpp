#include "recordlab/flowagent/agents/legacy_remote_action_client.h"

#include <zmq.hpp>

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <unistd.h>

namespace recordlab::flowagent::agents {

LegacyRemoteActionClient::LegacyRemoteActionClient(std::string host,
                                                   int goalPort,
                                                   int feedbackPort,
                                                   int requestTimeoutMs)
    : host_(std::move(host)), goalPort_(goalPort),
      feedbackPort_(feedbackPort), requestTimeoutMs_(requestTimeoutMs),
      context_(std::make_unique<zmq::context_t>(1)) {
  resetGoalSocket();

  feedbackSocket_ = std::make_unique<zmq::socket_t>(*context_, ZMQ_SUB);
  int lingerMs = 0;
  int receiveTimeoutMs = 100;
  feedbackSocket_->set(zmq::sockopt::linger, lingerMs);
  feedbackSocket_->set(zmq::sockopt::rcvtimeo, receiveTimeoutMs);
  feedbackSocket_->set(zmq::sockopt::subscribe, "");
  feedbackSocket_->connect("tcp://" + host_ + ":" + std::to_string(feedbackPort_));

  running_ = true;
  listenerThread_ = std::thread(&LegacyRemoteActionClient::listenLoop, this);
}

LegacyRemoteActionClient::~LegacyRemoteActionClient() {
  running_ = false;
  if (listenerThread_.joinable()) {
    listenerThread_.join();
  }
  if (goalSocket_) {
    goalSocket_->close();
  }
  if (feedbackSocket_) {
    feedbackSocket_->close();
  }
  if (context_) {
    context_->close();
  }
}

bool LegacyRemoteActionClient::waitForServer(int timeoutMs,
                                             std::string *errorMessage) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  std::string lastError;

  while (std::chrono::steady_clock::now() < deadline) {
    try {
      const uint32_t goalId = sendGoal(nlohmann::json{{"ping", true}});
      if (waitForResult(goalId, 1000)) {
        removeGoal(goalId);
        return true;
      }
      removeGoal(goalId);
      lastError = "ping result timeout";
    } catch (const std::exception &e) {
      lastError = e.what();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  if (errorMessage) {
    *errorMessage = lastError.empty() ? "server timeout" : lastError;
  }
  return false;
}

uint32_t LegacyRemoteActionClient::sendGoal(
    const nlohmann::json &goal, FeedbackCallback feedbackCallback,
    ResultCallback resultCallback) {
  const uint32_t localGoalId = nextGoalId_.fetch_add(1);
  const std::string remoteGoalId = makeRemoteGoalId(localGoalId);

  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingByLocalId_[localGoalId] =
        PendingGoal{remoteGoalId, std::move(feedbackCallback),
                    std::move(resultCallback), nlohmann::json::object(),
                    false, false};
    localIdByRemoteId_[remoteGoalId] = localGoalId;
  }

  try {
    const nlohmann::json response =
        callGoalSocket({{"goal_id", remoteGoalId}, {"goal", goal}});
    const bool accepted =
        response.value("status", std::string()) == "ACCEPTED" ||
        response.value("accepted", false);
    if (!accepted) {
      removeGoal(localGoalId);
      throw std::runtime_error("Goal rejected: " + response.dump());
    }
  } catch (...) {
    removeGoal(localGoalId);
    throw;
  }

  return localGoalId;
}

bool LegacyRemoteActionClient::waitForResult(uint32_t goalId, int timeoutMs) {
  std::unique_lock<std::mutex> lock(pendingMutex_);
  const auto done = [this, goalId]() {
    auto it = pendingByLocalId_.find(goalId);
    return it != pendingByLocalId_.end() && it->second.resultReceived;
  };

  if (timeoutMs <= 0) {
    pendingCv_.wait(lock, done);
    return done();
  }
  return pendingCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), done);
}

std::string LegacyRemoteActionClient::makeRemoteGoalId(
    uint32_t localGoalId) const {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::ostringstream os;
  os << "recordlabc-" << getpid() << "-" << now << "-" << localGoalId;
  return os.str();
}

nlohmann::json
LegacyRemoteActionClient::callGoalSocket(const nlohmann::json &request) {
  std::lock_guard<std::mutex> lock(socketMutex_);
  try {
    const std::string payload = request.dump();
    zmq::message_t requestMessage(payload.data(), payload.size());
    if (!goalSocket_->send(requestMessage, zmq::send_flags::none)) {
      resetGoalSocket();
      throw std::runtime_error("failed to send goal request");
    }

    zmq::message_t responseMessage;
    const auto received =
        goalSocket_->recv(responseMessage, zmq::recv_flags::none);
    if (!received) {
      resetGoalSocket();
      throw std::runtime_error("goal request timeout");
    }

    const std::string responseText(
        static_cast<const char *>(responseMessage.data()),
        responseMessage.size());
    return nlohmann::json::parse(responseText);
  } catch (...) {
    resetGoalSocket();
    throw;
  }
}

void LegacyRemoteActionClient::resetGoalSocket() {
  if (goalSocket_) {
    goalSocket_->close();
  }
  goalSocket_ = std::make_unique<zmq::socket_t>(*context_, ZMQ_REQ);
  int lingerMs = 0;
  goalSocket_->set(zmq::sockopt::linger, lingerMs);
  goalSocket_->set(zmq::sockopt::sndtimeo, requestTimeoutMs_);
  goalSocket_->set(zmq::sockopt::rcvtimeo, requestTimeoutMs_);
  goalSocket_->connect("tcp://" + host_ + ":" + std::to_string(goalPort_));
}

void LegacyRemoteActionClient::listenLoop() {
  while (running_) {
    try {
      std::vector<std::string> parts;
      while (running_) {
        zmq::message_t message;
        const auto received =
            feedbackSocket_->recv(message, zmq::recv_flags::none);
        if (!received) {
          break;
        }
        parts.emplace_back(static_cast<const char *>(message.data()),
                           message.size());

        const bool more = feedbackSocket_->get(zmq::sockopt::rcvmore);
        if (!more) {
          break;
        }
      }

      if (parts.empty()) {
        continue;
      }

      const std::string &payload = parts.size() == 1 ? parts.front() : parts.back();
      const auto message = nlohmann::json::parse(payload);
      const std::string type = message.value("type", std::string());
      if (type == "feedback") {
        handleFeedback(message);
      } else if (type == "result") {
        handleResult(message);
      }
    } catch (...) {
    }
  }
}

void LegacyRemoteActionClient::handleFeedback(const nlohmann::json &message) {
  const std::string remoteGoalId = message.value("goal_id", std::string());
  nlohmann::json feedback = message.value("feedback", nlohmann::json::object());
  FeedbackCallback callback;
  uint32_t localGoalId = 0;

  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    auto idIt = localIdByRemoteId_.find(remoteGoalId);
    if (idIt == localIdByRemoteId_.end()) {
      return;
    }
    localGoalId = idIt->second;
    auto pendingIt = pendingByLocalId_.find(localGoalId);
    if (pendingIt == pendingByLocalId_.end()) {
      return;
    }
    callback = pendingIt->second.feedbackCallback;
  }

  if (callback) {
    callback(localGoalId, feedback);
  }
}

void LegacyRemoteActionClient::handleResult(const nlohmann::json &message) {
  const std::string remoteGoalId = message.value("goal_id", std::string());
  nlohmann::json result = message.value("result", nlohmann::json::object());
  const std::string status = message.value("status", std::string());
  const bool success = status == "SUCCEEDED";
  ResultCallback callback;
  uint32_t localGoalId = 0;

  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    auto idIt = localIdByRemoteId_.find(remoteGoalId);
    if (idIt == localIdByRemoteId_.end()) {
      return;
    }
    localGoalId = idIt->second;
    auto pendingIt = pendingByLocalId_.find(localGoalId);
    if (pendingIt == pendingByLocalId_.end()) {
      return;
    }
    pendingIt->second.result = result;
    pendingIt->second.success = success;
    pendingIt->second.resultReceived = true;
    callback = pendingIt->second.resultCallback;
  }

  pendingCv_.notify_all();
  if (callback) {
    callback(localGoalId, result, success);
  }
}

void LegacyRemoteActionClient::removeGoal(uint32_t goalId) {
  std::lock_guard<std::mutex> lock(pendingMutex_);
  auto it = pendingByLocalId_.find(goalId);
  if (it != pendingByLocalId_.end()) {
    localIdByRemoteId_.erase(it->second.remoteGoalId);
    pendingByLocalId_.erase(it);
  }
}

} // namespace recordlab::flowagent::agents
