#include "builder/ResponseBuilder.h"
#include "handler/AppHandlers.h"
#include <fstream>
#include <sstream>

ResponseBuilder::ResponseBuilder() : response_(std::make_shared<HttpResponse>()) {}

ResponseBuilder::ResponseBuilder(std::shared_ptr<HttpResponse> resp) : response_(std::move(resp)) {
  if (!response_) {
    response_ = std::make_shared<HttpResponse>();
  }
}

std::shared_ptr<HttpResponse> ResponseBuilder::Create(HttpStatusCode code) {
  return std::make_shared<HttpResponse>(code);
}

std::shared_ptr<HttpResponse> ResponseBuilder::WithBody(const std::string& body,
                                                        const std::string& contentType) {
  auto resp = std::make_shared<HttpResponse>(HttpStatusCode::OK);
  resp->SetHeader("Content-Type", contentType);
  resp->SetBody(body);
  return resp;
}

ResponseBuilder ResponseBuilder::New() {
  return ResponseBuilder();
}

ResponseBuilder& ResponseBuilder::Status(HttpStatusCode code) {
  response_->SetStatusCode(code);
  return *this;
}

ResponseBuilder& ResponseBuilder::Status(int code, std::string_view reason) {
  response_->SetStatusLine(response_->GetVersion(), code, reason);
  return *this;
}

ResponseBuilder& ResponseBuilder::Reason(std::string_view reason) {
  response_->SetStatusReason(reason);
  return *this;
}

ResponseBuilder& ResponseBuilder::Version(HttpVersion version) {
  response_->SetVersion(version);
  return *this;
}

ResponseBuilder& ResponseBuilder::Header(const std::string& key, const std::string& value) {
  response_->AppendHeader(key, value);
  return *this;
}

ResponseBuilder& ResponseBuilder::Headers(const std::vector<std::pair<std::string, std::string>>& headers) {
  response_->AppendHeader(headers);
  return *this;
}

ResponseBuilder& ResponseBuilder::ContentType(const std::string& contentType) {
  response_->SetHeader("Content-Type", contentType);
  return *this;
}

ResponseBuilder& ResponseBuilder::ContentEncoding(HttpContentEncoding encoding) {
  response_->SetContentEncoding(encoding);
  return *this;
}

ResponseBuilder& ResponseBuilder::Body(const std::string& body) {
  response_->SetBody(body);
  return *this;
}

std::shared_ptr<HttpResponse> ResponseBuilder::Build() {
  return response_;
}

ResponseBuilder& ResponseBuilder::Json(bool success, const std::string& message) {
  std::ostringstream json;
  json << "{\"success\":" << (success ? "true" : "false")
       << ",\"message\":\"" << message << "\"}";
  
  response_->SetHeader("Content-Type", "application/json; charset=utf-8");
  response_->SetBody(json.str());
  return *this;
}

ResponseBuilder& ResponseBuilder::Json(bool success, const std::string& message, const std::string& data) {
  std::ostringstream json;
  json << "{\"success\":" << (success ? "true" : "false")
       << ",\"message\":\"" << message << "\""
       << ",\"data\":" << data << "}";
  
  response_->SetHeader("Content-Type", "application/json; charset=utf-8");
  response_->SetBody(json.str());
  return *this;
}

ResponseBuilder& ResponseBuilder::Json(const std::string& jsonBody) {
  response_->SetHeader("Content-Type", "application/json; charset=utf-8");
  response_->SetBody(jsonBody);
  return *this;
}

ResponseBuilder& ResponseBuilder::Error(HttpStatusCode code, const std::string& message) {
  response_->SetStatusCode(code);
  std::ostringstream json;
  json << "{\"success\":false,\"error\":{\"code\":" << static_cast<int>(code)
       << ",\"message\":\"" << message << "\"}}";
  
  response_->SetHeader("Content-Type", "application/json; charset=utf-8");
  response_->SetBody(json.str());
  return *this;
}

ResponseBuilder& ResponseBuilder::Error(int code, const std::string& message, const std::string& reason) {
  response_->SetStatusLine(response_->GetVersion(), code, reason.empty() ? "Error" : reason);
  std::ostringstream json;
  json << "{\"success\":false,\"error\":{\"code\":" << code
       << ",\"message\":\"" << message << "\"}}";
  
  response_->SetHeader("Content-Type", "application/json; charset=utf-8");
  response_->SetBody(json.str());
  return *this;
}

ResponseBuilder& ResponseBuilder::FileDownload(const std::string& filename, const std::string& filepath) {
  std::ifstream file(filepath, std::ios::binary);
  if (!file.is_open()) {
    return Error(HttpStatusCode::INTERNAL_SERVER_ERROR, "无法打开文件");
  }

  std::string file_content((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
  file.close();

  response_->SetStatusCode(HttpStatusCode::OK);
  response_->SetBody(file_content);
  response_->SetHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  response_->SetHeader("Content-Length", std::to_string(file_content.size()));
  
  return *this;
}

ResponseBuilder& ResponseBuilder::StaticFile(const std::string& filepath, const std::string& contentType) {
  std::ifstream file(filepath, std::ios::binary);
  if (!file.is_open()) {
    return Error(HttpStatusCode::NOT_FOUND, "文件不存在");
  }

  std::string file_content((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
  file.close();

  response_->SetStatusCode(HttpStatusCode::OK);
  response_->SetBody(file_content);
  
  if (!contentType.empty()) {
    response_->SetHeader("Content-Type", contentType);
  } else {
    response_->SetHeader("Content-Type", ::GetContentType(filepath));
  }
  
  response_->SetHeader("Content-Length", std::to_string(file_content.size()));
  return *this;
}

ResponseBuilder& ResponseBuilder::Redirect(const std::string& url, bool permanent) {
  if (permanent) {
    response_->SetStatusCode(HttpStatusCode::MOVED_PERMANENTLY);
  } else {
    response_->SetStatusCode(HttpStatusCode::FOUND);
  }
  response_->SetHeader("Location", url);
  return *this;
}
