#include "builder/RequestBuilder.h"

RequestBuilder::RequestBuilder() : request_(std::make_shared<HttpRequest>()) {}

RequestBuilder::RequestBuilder(std::shared_ptr<HttpRequest> req) : request_(std::move(req)) {
  if (!request_) {
    request_ = std::make_shared<HttpRequest>();
  }
}

std::shared_ptr<HttpRequest> RequestBuilder::CreateEmpty() {
  return std::make_shared<HttpRequest>();
}

std::shared_ptr<HttpRequest> RequestBuilder::FromRaw(const std::string& raw) {
  auto req = std::make_shared<HttpRequest>();
  req->SetBody(raw);
  return req;
}

RequestBuilder RequestBuilder::New() {
  return RequestBuilder();
}

RequestBuilder& RequestBuilder::Method(HttpMethod method) {
  request_->SetMethod(method);
  return *this;
}

RequestBuilder& RequestBuilder::Method(std::string_view methodStr) {
  request_->SetMethodString(methodStr);
  return *this;
}

RequestBuilder& RequestBuilder::Url(std::string_view url) {
  request_->SetUrl(url);
  return *this;
}

RequestBuilder& RequestBuilder::Path(std::string_view path) {
  request_->SetPath(path);
  return *this;
}

RequestBuilder& RequestBuilder::Version(HttpVersion version) {
  request_->SetVersion(version);
  return *this;
}

RequestBuilder& RequestBuilder::Header(const std::string& key, const std::string& value) {
  request_->AppendHeader(key, value);
  return *this;
}

RequestBuilder& RequestBuilder::Headers(const std::vector<std::pair<std::string, std::string>>& headers) {
  request_->AppendHeader(headers);
  return *this;
}

RequestBuilder& RequestBuilder::QueryParam(const std::string& key, const std::string& value) {
  request_->AddQueryParam(key, value);
  return *this;
}

RequestBuilder& RequestBuilder::Body(const std::string& body) {
  request_->SetBody(body);
  return *this;
}

std::shared_ptr<HttpRequest> RequestBuilder::Build() {
  return request_;
}
