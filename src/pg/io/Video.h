#pragma once
//
// Video files, a frame at a time: what a shot is played and handed on as.
//
//   .avi                       Motion JPEG, written here (Jpeg.h) -- no program
//                              needed; VLC, mpv, ffmpeg, the editors play it
//   .mp4 .mov .mkv .webm .gif  through ffmpeg, when it is installed: H.264
//                              (or MPEG-4 part 2), VP9 (or VP8), a GIF with a
//                              palette of its own. PG_FFMPEG names the program
//                              if it is not `ffmpeg` on the PATH.
//
// Frames are RGB, three bytes a pixel, the top row first -- as the renderers
// read them back. H.264 and VP9 want an even size: an odd row or column is
// added, repeating the last.
//
// With alpha -- a transparent render -- frames are RGBA, four bytes a pixel,
// the colour not premultiplied: kept by .mov (ProRes 4444, or QuickTime
// Animation without it), .webm (VP9 with alpha) and .mkv (FFV1, lossless).
//
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pg::io {

class VideoWriter {
public:
    virtual ~VideoWriter() = default;
    /// One frame of width x height pixels. False, with why, when it could
    /// not be written; the file is then of no use.
    virtual bool add(const uint8_t* rgb, std::string& error) = 0;
    /// Finishes the file. False, with why, if it did not come out.
    virtual bool finish(std::string& error) = 0;
    int frames() const { return frames_; }
    /// "Motion JPEG", "H.264 (ffmpeg)"...
    virtual std::string codec() const = 0;

protected:
    int frames_ = 0;
};

/// A writer for `path`, by its extension, of frames of width x height at
/// `fps`; null, with why, if the extension is not a video's, ffmpeg is
/// wanted and not there, or the file cannot be made.
/// `alpha`: frames of four channels, into a file that keeps them
/// (videoKeepsAlpha) -- else null, with why.
std::unique_ptr<VideoWriter> openVideo(const std::string& path, int width, int height, double fps, std::string& error,
                                       bool alpha = false);
/// True for the extension of a video that keeps an alpha channel: .mov,
/// .webm, .mkv.
bool videoKeepsAlpha(const std::string& path);

/// True for the extension of a video file (.avi, .mp4, .mov, .mkv, .webm, .gif).
bool isVideoPath(const std::string& path);
/// The extensions a video can be written to now, with their dots: .avi
/// always; .mp4, .mov, .mkv, .webm and .gif when ffmpeg is there -- .mp4
/// first then.
std::vector<std::string> videoExtensions();
/// Whether ffmpeg runs (asked once).
bool ffmpegAvailable();

/// A frame rate as a fraction, rate / scale: 30 -> 30/1, 29.97 -> 2997/100.
void frameRate(double fps, uint32_t& rate, uint32_t& scale);

}  // namespace pg::io
