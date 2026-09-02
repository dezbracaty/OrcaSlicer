#ifndef slic3r_BeltTaperedSupport_hpp_
#define slic3r_BeltTaperedSupport_hpp_

namespace Slic3r {

class PrintObject;
struct SlicingParameters;

// Generates build-plate rooted support for a belt printer without modifying the
// source mesh. The generated support is expressed in the same oriented U/V/s
// coordinate system as the sliced PrintObject and is installed as SupportLayer
// instances on that object.
class BeltTaperedSupport
{
public:
    BeltTaperedSupport(PrintObject& object, const SlicingParameters& slicing_parameters);

    void generate();

private:
    PrintObject&             m_object;
    const SlicingParameters& m_slicing_parameters;
};

} // namespace Slic3r

#endif
