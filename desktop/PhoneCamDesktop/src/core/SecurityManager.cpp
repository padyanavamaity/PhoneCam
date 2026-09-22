#include <string>
#include <cstddef>
namespace phonecam {
class SecurityManager {
public:
 bool validateId(const std::string& s) const { return !s.empty() && s.size()<=128; }
 bool validatePayload(const std::string& s) const { return s.size()<=65536; }
 bool validateGain(float g) const { return g>=0.0f && g<=4.0f; }
};
}
