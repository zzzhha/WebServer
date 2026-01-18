#include "builder/ResponseBuilder.h"

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
