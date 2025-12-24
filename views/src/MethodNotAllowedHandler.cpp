#include "MethodNotAllowedHandler.h"
#include "../../http/include/core/HttpRequest.h"
#include "../../http/include/core/HttpResponse.h"
#include <sstream>

// 将HttpMethod转换为字符串
static std::string MethodToString(HttpMethod method) {
    switch (method) {
        case HttpMethod::GET: return "GET";
        case HttpMethod::POST: return "POST";
        case HttpMethod::PUT: return "PUT";
        case HttpMethod::DELETE: return "DELETE";
        case HttpMethod::PATCH: return "PATCH";
        case HttpMethod::HEAD: return "HEAD";
        case HttpMethod::OPTIONS: return "OPTIONS";
        case HttpMethod::TRACE: return "TRACE";
        case HttpMethod::CONNECT: return "CONNECT";
        default: return "UNKNOWN";
    }
}

// 处理405错误
void MethodNotAllowedHandler::Handle(HttpRequest* request, HttpResponse& response, 
                                     const std::vector<HttpMethod>& allowedMethods) {
    // 构建Allow响应头的值
    std::ostringstream allow_header;
    for (size_t i = 0; i < allowedMethods.size(); ++i) {
        if (i > 0) allow_header << ", ";
        allow_header << MethodToString(allowedMethods[i]);
    }
    
    std::string default_body = 
        "<html><head><title>405 Method Not Allowed</title></head>"
        "<body><h1>405 Method Not Allowed</h1>"
        "<p>The requested method is not allowed for this resource.</p>"
        "<p>Allowed methods: " + allow_header.str() + "</p>"
        "</body></html>";
    
    // 先调用基类的HandleError方法
    HandleError(request, response, 405, "405.html", default_body);
    
    // 设置Allow响应头
    response.SetHeader("Allow", allow_header.str());
}

