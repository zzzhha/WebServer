#include "BadRequestHandler.h"
#include "../../http/include/core/HttpRequest.h"
#include "../../http/include/core/HttpResponse.h"

// 处理400错误
void BadRequestHandler::Handle(HttpRequest* request, HttpResponse& response) {
    std::string default_body = 
        "<html><head><title>400 Bad Request</title></head>"
        "<body><h1>400 Bad Request</h1>"
        "<p>The request could not be understood by the server.</p>"
        "</body></html>";
    
    HandleError(request, response, 400, "400.html", default_body);
}

