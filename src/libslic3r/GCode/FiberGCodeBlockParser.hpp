#ifndef slic3r_GCode_FiberGCodeBlockParser_hpp_
#define slic3r_GCode_FiberGCodeBlockParser_hpp_

#include <stdexcept>
#include <string_view>
#include <string>
#include <sstream>
#include <cmath>
#include <cstdint>

namespace Slic3r {

struct FiberGCodeBlockLine {
    bool protected_line { false };
    bool begins_block { false };
    bool ends_block { false };
};

// Shared parser for all post-processors which may reorder or rewrite G-code.
// Marker lines belong to the protected block so no command may be inserted
// between FIBER_BEGIN / FIBER_END and the physical process they delimit.
class FiberGCodeBlockParser {
public:
    FiberGCodeBlockLine consume(std::string_view line)
    {
        if (is_marker(line, ";FIBER_BEGIN")) {
            if (m_inside)
                throw std::runtime_error("Nested FIBER_BEGIN marker");
            m_inside = true;
            return {true, true, false};
        }
        if (is_marker(line, ";FIBER_END")) {
            if (!m_inside)
                throw std::runtime_error("FIBER_END marker without FIBER_BEGIN");
            m_inside = false;
            return {true, false, true};
        }
        return {m_inside, false, false};
    }

    bool inside_block() const { return m_inside; }

private:
    static bool is_marker(std::string_view line, std::string_view marker)
    {
        return line.size() >= marker.size() &&
            line.substr(0, marker.size()) == marker &&
            (line.size() == marker.size() || line[marker.size()] == ' ' ||
             line[marker.size()] == '\t' || line[marker.size()] == '\r');
    }

    bool m_inside { false };
};


enum class ToolpathDeposition : uint8_t {
    None, Thermoplastic, ContinuousFiberPowered, ContinuousFiberPassive
};
enum class FiberProcessPhase : uint8_t {
    None, Approach, Prefeed, Ready, Landing, Powered, Cut, Tail, Depleted, Finish, Complete
};

// Semantic parser shared by generated and imported G-code. Physical motion
// classification stays independent of whether a move deposits fiber.
class FiberGCodeSemanticParser {
public:
    FiberProcessPhase phase {FiberProcessPhase::None};
    bool contour {false};
    double width {0}, height {0}, e_units_per_mm {1};
    uint64_t occurrence {0};

    bool inside() const { return phase != FiberProcessPhase::None; }
    ToolpathDeposition deposition() const {
        if (phase == FiberProcessPhase::Powered) return ToolpathDeposition::ContinuousFiberPowered;
        if (phase == FiberProcessPhase::Landing || phase == FiberProcessPhase::Tail)
            return ToolpathDeposition::ContinuousFiberPassive;
        return ToolpathDeposition::None;
    }
    bool consume(std::string_view comment) {
        if (comment.substr(0, 6) != "FIBER_") return false;
        std::istringstream input{std::string(comment)};
        std::string marker; input >> marker;
        const auto require = [](bool ok) {
            if (!ok) throw std::runtime_error("Malformed or out-of-order Fiber v3 process marker");
        };
        if (marker == "FIBER_BEGIN") {
            require(!inside());
            int version = 0; bool purpose = false;
            width = height = 0; e_units_per_mm = 0; occurrence = 0;
            std::string item;
            while (input >> item) {
                auto equal = item.find('=');
                if (equal == std::string::npos) continue;
                const auto key = item.substr(0, equal), value = item.substr(equal+1);
                if (key == "v") version = std::stoi(value);
                else if (key == "purpose") { require(value == "contour" || value == "infill"); contour = value == "contour"; purpose = true; }
                else if (key == "width") width = std::stod(value);
                else if (key == "height") height = std::stod(value);
                else if (key == "e_units_per_mm") e_units_per_mm = std::stod(value);
                else if (key == "occurrence") occurrence = std::stoull(value);
            }
            if (version != 3) throw std::runtime_error("Fiber preview requires v3 landing/deposition metadata; legacy or unknown protocol");
            require(purpose && occurrence > 0 && std::isfinite(width) && width > 0 &&
                    std::isfinite(height) && height > 0 && std::isfinite(e_units_per_mm) && e_units_per_mm > 0);
            phase = FiberProcessPhase::Approach;
        } else if (marker == "FIBER_PREFEED_BEGIN") {
            require(phase == FiberProcessPhase::Approach); phase = FiberProcessPhase::Prefeed;
        } else if (marker == "FIBER_PREFEED_END") {
            require(phase == FiberProcessPhase::Prefeed); phase = FiberProcessPhase::Ready;
        } else if (marker == "FIBER_LANDING_BEGIN") {
            require(phase == FiberProcessPhase::Ready || phase == FiberProcessPhase::Approach);
            phase = FiberProcessPhase::Landing;
        } else if (marker == "FIBER_LANDING_END") {
            require(phase == FiberProcessPhase::Landing); phase = FiberProcessPhase::Ready;
        } else if (marker == "FIBER_START") {
            require(phase == FiberProcessPhase::Ready || phase == FiberProcessPhase::Approach);
            phase = FiberProcessPhase::Powered;
        } else if (marker == "FIBER_CUT") {
            require(phase == FiberProcessPhase::Powered); phase = FiberProcessPhase::Cut;
        } else if (marker == "FIBER_TAIL_BEGIN") {
            require(phase == FiberProcessPhase::Cut); phase = FiberProcessPhase::Tail;
        } else if (marker == "FIBER_DEPLETED") {
            require(phase == FiberProcessPhase::Tail); phase = FiberProcessPhase::Depleted;
        } else if (marker == "FIBER_FINISH_BEGIN") {
            require(phase == FiberProcessPhase::Depleted); phase = FiberProcessPhase::Finish;
        } else if (marker == "FIBER_FINISH") {
            require(phase == FiberProcessPhase::Finish); phase = FiberProcessPhase::Complete;
        } else if (marker == "FIBER_END") {
            require(phase == FiberProcessPhase::Complete); phase = FiberProcessPhase::None; occurrence = 0;
        } else {
            throw std::runtime_error("Unknown Fiber process marker");
        }
        return true;
    }
    void finish() const {
        if (inside()) throw std::runtime_error("Unclosed Fiber process block");
    }
};

} // namespace Slic3r

#endif
