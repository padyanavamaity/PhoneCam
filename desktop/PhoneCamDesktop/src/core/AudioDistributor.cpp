#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
namespace phonecam {
struct AudioFrame { std::string deviceId; std::vector<std::uint8_t> pcm; int sampleRate=48000; int channels=2; };
class AudioDistributor {
public: void push(AudioFrame frame){ frames_[frame.deviceId]=std::move(frame); }
private: std::unordered_map<std::string,AudioFrame> frames_;
};
}
