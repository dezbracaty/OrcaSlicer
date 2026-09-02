#ifndef slic3r_BeltSupportDebug_hpp_
#define slic3r_BeltSupportDebug_hpp_

#include "libslic3r/Point.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

enum class BeltSupportDebugStageId : std::size_t
{
    InputSlices = 0,
    OverhangRegions,
    RawContacts,
    ConsolidatedContacts,
    RootProjection,
    ReachableCorridors,
    RouteClassification,
    TreeTopology,
    TaperedPrimitives,
    LayerSections,
    PathInputs,
    FinalSupport,
    Count
};

struct BeltSupportDebugLine
{
    Vec3d       start_world{Vec3d::Zero()};
    Vec3d       end_world{Vec3d::Zero()};
    std::string category;
};

struct BeltSupportDebugRecord
{
    std::string kind;
    std::string reason;
    std::map<std::string, double> values;
};

struct BeltSupportDebugStage
{
    std::string id;
    std::string name;
    std::string purpose;
    std::string expected;
    double elapsed_ms{0.0};
    std::map<std::string, double> metrics;
    std::vector<BeltSupportDebugLine> lines;
    std::vector<BeltSupportDebugRecord> records;
};

class BeltSupportDebugRecorder
{
public:
    BeltSupportDebugRecorder()
    {
        static const std::array<BeltSupportDebugStage, static_cast<std::size_t>(BeltSupportDebugStageId::Count)>
            definitions{{
                {"input_slices", "1. 输入模型切层", "显示支撑算法实际读取的定向切层轮廓。",
                 "轮廓应与模型重合，层面法向必须是 Belt 制造方向 N。"},
                {"overhang_regions", "2. 过悬区域", "显示检测器原始悬垂以及同层几何并集。",
                 "原始事实永久保留；同一物理区域在同层必须并集去重后才能进入接触采样。"},
                {"raw_contacts", "3. 原始接触采样", "在同层并集后的完整过悬区域中按配置间距采样，并叠加显示内缩安全核作为诊断证据。",
                 "采样点必须位于并集后的过悬区域内；薄区域即使无法形成完整内缩核也不能被静默丢弃。"},
                {"consolidated_contacts", "4. 有效接触点", "把相邻切层上的连续悬垂区域连接成表面分量，并按配置接触间距选择有效接触。",
                 "原始采样必须全部保留并显式映射到有效接触；映射不得跨越不连续表面，距离不得超过配置间距半径。"},
                {"root_projection", "5. 根投影", "沿 -N 把每个有效接触点投影到世界带面 Z=0。",
                 "每条线起于接触点、止于 Z=0，方向必须与 45° 龙门方向平行。"},
                {"reachable_corridors", "6. 逐层可达走廊", "显示分支在侧向角和模型避让约束下逐层仍可到达的 U 区间与 U/V 区域。",
                 "每个可达集合必须来自上一层按侧向角预算扩张后减去本层真实阻挡；禁止固定根点穷举或有损简化权威边界。"},
                {"route_classification", "7. 路径可达性", "显示模型阻挡区间以及每个接触点的路径回溯结果。",
                 "可达路径必须全程位于逐层走廊内，且 U/V 合成侧向斜率不得超过配置分支角；不可达路径必须记录具体失败层。"},
                {"tree_topology", "8. 合并后树拓扑", "显示走廊内简化后的分支、稳定合并点和真正到达带面的根干。",
                 "相邻分支只在连续三层截面重叠后合并；合并后的子分支不得继续各自生成根。"},
                {"tapered_primitives", "9. 锥台骨架", "显示后续解析矩形锥台的中心线和端部截面。",
                 "截面从接触端向根部单调增大，中心以 N 为主生长方向，U/V 合成偏移必须位于配置侧向分支圆锥内。"},
                {"layer_sections", "10. 逐层截面", "把解析锥台与每一个真实 45° 层面求交。",
                 "每个截面应围绕对应骨架中心并裁到世界 Z>=0；可达见证点必须被实际支撑覆盖；build-plate-only 下被模型封闭而无法落地的点必须逐点保留排除证据，不得穿模补齐。"},
                {"path_inputs", "11. 路径生成输入", "显示根线、普通区、接触区以及由接触区实际生成的填充路径。",
                 "首个非空输入只能是一条开放根线；接口必须生成覆盖区域的真实填充路径，不能只有闭合轮廓；审计数据永久保留。"},
                {"final_support", "12. 最终支撑路径", "显示真正安装到 SupportLayer 的根线、树干和接口挤出路径。",
                 "首层必须只有一条世界 Z=0 的开放路径；后续每个真实挤出足迹必须连接前一物理层、同层已落地路径或世界带面，不得用计划区域代替真实路径证明连通。"},
            }};
        m_stages.assign(definitions.begin(), definitions.end());
    }

    BeltSupportDebugStage& stage(BeltSupportDebugStageId id)
    {
        return m_stages.at(static_cast<std::size_t>(id));
    }

    const BeltSupportDebugStage& stage(BeltSupportDebugStageId id) const
    {
        return m_stages.at(static_cast<std::size_t>(id));
    }

    const std::vector<BeltSupportDebugStage>& stages() const noexcept { return m_stages; }

    void add_line(BeltSupportDebugStageId id, const Vec3d& start, const Vec3d& end,
                  std::string category)
    {
        stage(id).lines.push_back({start, end, std::move(category)});
    }

    void add_point(BeltSupportDebugStageId id, const Vec3d& point, double radius,
                   const std::string& category)
    {
        const Vec3d dx(radius, 0.0, 0.0);
        const Vec3d dy(0.0, radius, 0.0);
        const Vec3d dz(0.0, 0.0, radius);
        add_line(id, point - dx, point + dx, category);
        add_line(id, point - dy, point + dy, category);
        add_line(id, point - dz, point + dz, category);
    }

    void set_metric(BeltSupportDebugStageId id, std::string name, double value)
    {
        stage(id).metrics[std::move(name)] = value;
    }

    void add_record(BeltSupportDebugStageId id, std::string kind,
                    std::string reason, std::map<std::string, double> values)
    {
        stage(id).records.push_back(
            {std::move(kind), std::move(reason), std::move(values)});
    }

    void add_elapsed(BeltSupportDebugStageId id, double elapsed_ms)
    {
        stage(id).elapsed_ms += elapsed_ms;
    }

private:
    std::vector<BeltSupportDebugStage> m_stages;
};

class BeltSupportDebugStageTimer
{
public:
    BeltSupportDebugStageTimer(BeltSupportDebugRecorder* recorder, BeltSupportDebugStageId stage)
        : m_recorder(recorder), m_stage(stage), m_start(std::chrono::steady_clock::now())
    {}

    ~BeltSupportDebugStageTimer()
    {
        if (m_recorder == nullptr)
            return;
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - m_start).count();
        m_recorder->add_elapsed(m_stage, elapsed);
    }

private:
    BeltSupportDebugRecorder* m_recorder;
    BeltSupportDebugStageId m_stage;
    std::chrono::steady_clock::time_point m_start;
};

inline thread_local BeltSupportDebugRecorder* g_belt_support_debug_recorder = nullptr;

class ScopedBeltSupportDebugRecorder
{
public:
    explicit ScopedBeltSupportDebugRecorder(BeltSupportDebugRecorder* recorder)
        : m_previous(g_belt_support_debug_recorder)
    {
        g_belt_support_debug_recorder = recorder;
    }

    ~ScopedBeltSupportDebugRecorder() { g_belt_support_debug_recorder = m_previous; }

private:
    BeltSupportDebugRecorder* m_previous;
};

inline BeltSupportDebugRecorder* current_belt_support_debug_recorder() noexcept
{
    return g_belt_support_debug_recorder;
}

} // namespace Slic3r

#endif
