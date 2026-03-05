#pragma once

#include <string>

class HttpRequest;
class HttpResponse;
class RouteParams;

class UploadService {
public:
  static bool HandleInit(HttpRequest* request, HttpResponse& response, const std::string& static_path);
  static bool HandleUploadPart(HttpRequest* request, HttpResponse& response, const RouteParams& params, const std::string& static_path);
  static bool HandleComplete(HttpRequest* request, HttpResponse& response, const RouteParams& params, const std::string& static_path);
};

