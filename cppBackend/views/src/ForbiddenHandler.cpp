#include "ForbiddenHandler.h"
#include "../../http/include/core/HttpRequest.h"
#include "../../http/include/core/HttpResponse.h"

// 处理403错误
void ForbiddenHandler::Handle(HttpRequest* request, HttpResponse& response) {
    std::string default_body = 
        "<html><head><title>403 Forbidden</title></head>"
        "<body><h1>403 Forbidden</h1>"
        "<p>You do not have permission to access this resource.</p>"
        "</body></html>";
    
    HandleError(request, response, 403, "403.html", default_body);
}

