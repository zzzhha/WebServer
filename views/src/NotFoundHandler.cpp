#include "NotFoundHandler.h"
#include "../../http/include/core/HttpRequest.h"
#include "../../http/include/core/HttpResponse.h"

// 处理404错误
void NotFoundHandler::Handle(HttpRequest* request, HttpResponse& response) {
    std::string default_body = 
        "<html><head><title>404 Not Found</title></head>"
        "<body><h1>404 Not Found</h1>"
        "<p>The requested resource was not found.</p>"
        "</body></html>";
    
    HandleError(request, response, 404, "404.html", default_body);
}

