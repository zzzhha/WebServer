#pragma once

#include <string>

class HttpRequest;
class HttpResponse;

/**
 * ThumbService：懒生成缩略图/视频封面。
 * GET /thumb/<folder>/<name>?w=<width>
 * 用 ffmpeg 生成（图片缩放；视频 thumbnail 滤镜选信息量最大帧，避免黑屏），
 * 落盘缓存到 _thumbs/<folder>/，并回写 files 表的 thumb_state / poster_state。
 */
class ThumbService {
 public:
  static bool HandleThumb(HttpRequest* request, HttpResponse& response, const std::string& static_path);
};
