#pragma once

#include <string>

class HttpRequest;
class HttpResponse;

class FileApiService {
public:
  static bool HandleListFiles(HttpRequest* request, HttpResponse& response, const std::string& static_path);
  static bool HandlePreview(HttpRequest* request, HttpResponse& response, const std::string& static_path);
};

