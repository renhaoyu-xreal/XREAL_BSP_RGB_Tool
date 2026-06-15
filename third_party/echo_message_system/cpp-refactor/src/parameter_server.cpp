#include "parameter_server.h"
#include "logger.h"
#include "context.h"
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>

namespace echo {

// ==================== ParameterServer ====================

ParameterServer::ParameterServer(int port) 
    : port_(port), running_(false) {
    context_ = ContextManager::getGlobalContext();
    socket_ = std::make_unique<zmq::socket_t>(*context_, ZMQ_REP);
}

ParameterServer::~ParameterServer() {
    stop();
}

void ParameterServer::start() {
    std::string endpoint = "tcp://*:" + std::to_string(port_);
    socket_->bind(endpoint);
    running_ = true;
    
    LOG_INFO("ParameterServer", "Parameter Server started on port ", port_);
    
    while (running_) {
        try {
            zmq::message_t request_msg;
            auto result = socket_->recv(&request_msg, ZMQ_DONTWAIT);
            
            if (result) {
                std::string req_str(static_cast<char*>(request_msg.data()), request_msg.size());
                nlohmann::json request = nlohmann::json::parse(req_str);
                
                nlohmann::json response;
                handleRequest(request, response);
                
                std::string resp_str = response.dump();
                zmq::message_t response_msg(resp_str.data(), resp_str.size());
                socket_->send(response_msg);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } catch (const std::exception& e) {
            LOG_ERROR("ParameterServer", "Error handling request: ", e.what());
        }
    }
    
    LOG_INFO("ParameterServer", "Parameter Server stopped");
}

void ParameterServer::stop() {
    running_ = false;
    if (socket_) {
        socket_->close();
    }
}

void ParameterServer::registerToMaster(const std::string& master_address, int master_port) {
    try {
        MasterClient& master = MasterClient::getInstance();
        bool success = master.registerService("/roslib_comm", port_);
        
        if (success) {
            LOG_INFO("ParameterServer", "Registered to Master at ", master_address, ":", master_port);
        } else {
            LOG_ERROR("ParameterServer", "Failed to register to Master");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("ParameterServer", "Error registering to Master: ", e.what());
    }
}

void ParameterServer::handleRequest(const nlohmann::json& request, nlohmann::json& response) {
    try {
        std::string type = request["type"];
        
        if (type == "set_param") {
            setParam(request, response);
        } else if (type == "get_param") {
            getParam(request, response);
        } else if (type == "delete_param") {
            deleteParam(request, response);
        } else if (type == "list_params") {
            listParams(request, response);
        } else if (type == "search_param") {
            searchParam(request, response);
        } else if (type == "has_param") {
            hasParam(request, response);
        } else {
            response["success"] = false;
            response["error"] = "Unknown request type: " + type;
        }
    } catch (const std::exception& e) {
        response["success"] = false;
        response["error"] = std::string(e.what());
    }
}

void ParameterServer::setParam(const nlohmann::json& req, nlohmann::json& resp) {
    std::lock_guard<std::mutex> lock(params_mutex_);
    
    std::string param_name = req["param_name"];
    nlohmann::json value = req["value"];
    
    parameters_[param_name] = value;
    
    LOG_INFO("ParameterServer", "Set parameter '", param_name, "' = ", value.dump());
    
    resp["success"] = true;
}

void ParameterServer::getParam(const nlohmann::json& req, nlohmann::json& resp) {
    std::lock_guard<std::mutex> lock(params_mutex_);
    
    std::string param_name = req["param_name"];
    
    auto it = parameters_.find(param_name);
    if (it != parameters_.end()) {
        resp["value"] = it->second;
        resp["success"] = true;
    } else {
        resp["success"] = false;
        resp["error"] = "Parameter not found: " + param_name;
    }
}

void ParameterServer::deleteParam(const nlohmann::json& req, nlohmann::json& resp) {
    std::lock_guard<std::mutex> lock(params_mutex_);
    
    std::string param_name = req["param_name"];
    
    auto it = parameters_.find(param_name);
    if (it != parameters_.end()) {
        parameters_.erase(it);
        LOG_INFO("ParameterServer", "Deleted parameter '", param_name, "'");
        resp["success"] = true;
    } else {
        resp["success"] = false;
        resp["error"] = "Parameter not found: " + param_name;
    }
}

void ParameterServer::listParams(const nlohmann::json& req, nlohmann::json& resp) {
    std::lock_guard<std::mutex> lock(params_mutex_);
    
    nlohmann::json params_list = nlohmann::json::array();
    for (const auto& [name, value] : parameters_) {
        params_list.push_back({
            {"name", name},
            {"value", value}
        });
    }
    
    resp["params"] = params_list;
    resp["count"] = params_list.size();
    resp["success"] = true;
}

void ParameterServer::searchParam(const nlohmann::json& req, nlohmann::json& resp) {
    std::lock_guard<std::mutex> lock(params_mutex_);
    
    std::string search_pattern = req["pattern"];
    
    nlohmann::json matched = nlohmann::json::array();
    for (const auto& [name, value] : parameters_) {
        if (StringUtils::matchPattern(name, search_pattern)) {
            matched.push_back({
                {"name", name},
                {"value", value}
            });
        }
    }
    
    resp["matched"] = matched;
    resp["count"] = matched.size();
    resp["success"] = true;
}

void ParameterServer::hasParam(const nlohmann::json& req, nlohmann::json& resp) {
    std::lock_guard<std::mutex> lock(params_mutex_);
    
    std::string param_name = req["param_name"];
    bool exists = parameters_.find(param_name) != parameters_.end();
    
    resp["exists"] = exists;
    resp["success"] = true;
}

// ==================== ParameterClient ====================

ParameterClient::ParameterClient(const std::string& param_server_address, int param_server_port)
    : param_server_address_(param_server_address), param_server_port_(param_server_port) {
    
    context_ = ContextManager::getGlobalContext();
    socket_ = std::make_unique<zmq::socket_t>(*context_, ZMQ_REQ);
    
    std::string endpoint = "tcp://" + param_server_address_ + ":" + std::to_string(param_server_port_);
    socket_->connect(endpoint);
    
    LOG_INFO("ParameterClient", "Connected to Parameter Server at ", endpoint);
}

ParameterClient::~ParameterClient() {
    if (socket_) {
        socket_->close();
    }
}

ParameterClient& ParameterClient::getInstance() {
    static ParameterClient instance;
    return instance;
}

nlohmann::json ParameterClient::sendRequest(const nlohmann::json& request) {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    
    std::string req_str = request.dump();
    zmq::message_t request_msg(req_str.data(), req_str.size());
    socket_->send(request_msg);
    
    zmq::message_t response_msg;
    auto result = socket_->recv(&response_msg);
    
    std::string resp_str(static_cast<char*>(response_msg.data()), response_msg.size());
    return nlohmann::json::parse(resp_str);
}

bool ParameterClient::setParam(const std::string& param_name, const nlohmann::json& value) {
    try {
        nlohmann::json request = {
            {"type", "set_param"},
            {"param_name", param_name},
            {"value", value}
        };
        
        nlohmann::json response = sendRequest(request);
        return response.value("success", false);
    } catch (const std::exception& e) {
        LOG_ERROR("ParameterClient", "Error setting parameter: ", e.what());
        return false;
    }
}

bool ParameterClient::getParam(const std::string& param_name, nlohmann::json& value) {
    try {
        nlohmann::json request = {
            {"type", "get_param"},
            {"param_name", param_name}
        };
        
        nlohmann::json response = sendRequest(request);
        
        if (response.value("success", false)) {
            value = response["value"];
            return true;
        }
        
        LOG_WARN("ParameterClient", "Parameter not found: ", param_name);
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("ParameterClient", "Error getting parameter: ", e.what());
        return false;
    }
}

bool ParameterClient::deleteParam(const std::string& param_name) {
    try {
        nlohmann::json request = {
            {"type", "delete_param"},
            {"param_name", param_name}
        };
        
        nlohmann::json response = sendRequest(request);
        return response.value("success", false);
    } catch (const std::exception& e) {
        LOG_ERROR("ParameterClient", "Error deleting parameter: ", e.what());
        return false;
    }
}

std::vector<std::string> ParameterClient::listParams() {
    try {
        nlohmann::json request = {
            {"type", "list_params"}
        };
        
        nlohmann::json response = sendRequest(request);
        
        std::vector<std::string> result;
        if (response.value("success", false) && response.contains("params")) {
            for (const auto& param : response["params"]) {
                result.push_back(param["name"]);
            }
        }
        
        return result;
    } catch (const std::exception& e) {
        LOG_ERROR("ParameterClient", "Error listing parameters: ", e.what());
        return {};
    }
}

std::vector<std::string> ParameterClient::searchParam(const std::string& search_pattern) {
    try {
        nlohmann::json request = {
            {"type", "search_param"},
            {"pattern", search_pattern}
        };
        
        nlohmann::json response = sendRequest(request);
        
        std::vector<std::string> result;
        if (response.value("success", false) && response.contains("matched")) {
            for (const auto& param : response["matched"]) {
                result.push_back(param["name"]);
            }
        }
        
        return result;
    } catch (const std::exception& e) {
        LOG_ERROR("ParameterClient", "Error searching parameters: ", e.what());
        return {};
    }
}

bool ParameterClient::hasParam(const std::string& param_name) {
    try {
        nlohmann::json request = {
            {"type", "has_param"},
            {"param_name", param_name}
        };
        
        nlohmann::json response = sendRequest(request);
        return response.value("exists", false);
    } catch (const std::exception& e) {
        LOG_ERROR("ParameterClient", "Error checking parameter existence: ", e.what());
        return false;
    }
}

bool ParameterClient::loadParamFile(const std::string& filename) {
    try {
        std::ifstream file(filename);
        if (!file.is_open()) {
            LOG_ERROR("ParameterClient", "Failed to open parameter file: ", filename);
            return false;
        }
        
        nlohmann::json params = nlohmann::json::parse(file);
        
        for (auto& [key, value] : params.items()) {
            if (!setParam(key, value)) {
                LOG_WARN("ParameterClient", "Failed to set parameter: ", key);
            }
        }
        
        LOG_INFO("ParameterClient", "Loaded parameters from: ", filename);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("ParameterClient", "Error loading parameter file: ", e.what());
        return false;
    }
}

bool ParameterClient::saveParamFile(const std::string& filename) {
    try {
        auto param_list = listParams();
        nlohmann::json params;
        
        for (const auto& param_name : param_list) {
            nlohmann::json value;
            if (getParam(param_name, value)) {
                params[param_name] = value;
            }
        }
        
        std::ofstream file(filename);
        if (!file.is_open()) {
            LOG_ERROR("ParameterClient", "Failed to open file for writing: ", filename);
            return false;
        }
        
        file << params.dump(4);  // 4-space indent
        file.close();
        
        LOG_INFO("ParameterClient", "Saved parameters to: ", filename);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("ParameterClient", "Error saving parameter file: ", e.what());
        return false;
    }
}



} // namespace echo