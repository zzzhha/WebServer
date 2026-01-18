#include "observer/LoggingObserver.h"
//#include "Logger.h"  // 外部日志系统
#include "core/IHttpMessage.h"
#include "core/HttpRequest.h"
#include <sstream>

void LoggingObserver::OnSslProcess(const std::string& event, const std::string& details) {
    std::string message = FormatLogMessage("SSL解析", event, details);
    
    // 根据事件类型选择日志级别
    if (event.find("失败") != std::string::npos || event.find("错误") != std::string::npos) {
        //ERROR(message);
    } else if (event.find("警告") != std::string::npos) {
        //WARNING(message);
    } else if (event.find("完成") != std::string::npos || event.find("成功") != std::string::npos) {
        //INFO(message);
    } else {
        //DEBUG(message);
    }
}

void LoggingObserver::OnHttpParse(const std::string& event, const std::string& details) {
    std::string message = FormatLogMessage("HTTP解析", event, details);
    
    if (event.find("失败") != std::string::npos || event.find("错误") != std::string::npos) {
        //ERROR(message);
    } else if (event.find("警告") != std::string::npos) {
        //WARNING(message);
    } else if (event.find("完成") != std::string::npos || event.find("成功") != std::string::npos) {
        //(message);
    } else {
        //DEBUG(message);
    }
}

void LoggingObserver::OnValidation(const std::string& event, const std::string& details) {
    std::string message = FormatLogMessage("责任链验证", event, details);
    
    if (event.find("失败") != std::string::npos || event.find("错误") != std::string::npos) {
        //ERROR(message);
    } else if (event.find("警告") != std::string::npos) {
        //WARNING(message);
    } else if (event.find("完成") != std::string::npos || event.find("成功") != std::string::npos) {
        //INFO(message);
    } else {
        //DEBUG(message);
    }
}

void LoggingObserver::OnRouting(const std::string& event, const std::string& details) {
    std::string message = FormatLogMessage("路由处理", event, details);
    
    if (event.find("失败") != std::string::npos || event.find("错误") != std::string::npos) {
        //ERROR(message);
    } else if (event.find("警告") != std::string::npos) {
        //WARNING(message);
    } else if (event.find("完成") != std::string::npos || event.find("成功") != std::string::npos) {
        //INFO(message);
    } else {
        //DEBUG(message);
    }
}

void LoggingObserver::OnMessage(const IHttpMessage& message) {
    std::ostringstream oss;
    oss << "[HttpServer] 处理完成 - ";
    
    if (message.IsRequest()) {
        oss << "请求消息";
        if (auto* req = dynamic_cast<const HttpRequest*>(&message)) {
            oss << " | 方法: " << req->GetMethodString()
                << " | 路径: " << req->GetPath()
                << " | 版本: " << req->GetVersionStr();
        }
    } else {
        oss << "响应消息";
    }
    
    //INFO(oss.str());
}

void LoggingObserver::OnStageEvent(HttpProcessStage stage, const std::string& event, const std::string& details) {
    std::string stageName;
    switch (stage) {
        case HttpProcessStage::SSL_PROCESS:
            stageName = "SSL解析";
            break;
        case HttpProcessStage::HTTP_PARSE:
            stageName = "HTTP解析";
            break;
        case HttpProcessStage::VALIDATION:
            stageName = "责任链验证";
            break;
        case HttpProcessStage::ROUTING:
            stageName = "路由处理";
            break;
        case HttpProcessStage::COMPLETE:
            stageName = "处理完成";
            break;
    }
    
    std::string message = FormatLogMessage(stageName, event, details);
    //DEBUG(message);
}

std::string LoggingObserver::FormatLogMessage(const std::string& stage, const std::string& event, const std::string& details) const {
    std::ostringstream oss;
    oss << "[" << stage << "] " << event;
    if (!details.empty()) {
        oss << " - " << details;
    }
    return oss.str();
}

void LoggingObserver::LogWithLevel(const std::string& level, const std::string& message) const {
    // 这个方法可以用于更细粒度的控制，目前通过直接调用宏实现
    // 保留作为扩展接口
}

