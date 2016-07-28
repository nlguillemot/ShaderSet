#pragma once

// Replace with your own GL header include
#include "opengl.h"

#include <vector>
#include <utility>
#include <map>
#include <set>

class ShaderSet
{
    using ShaderNameTypePair = std::pair<std::string, GLenum>;
    using ShaderHandleTimestampPair = std::pair<GLuint, uint64_t>;
    using ProgramHandle = GLuint;

    std::string mVersion;
    std::string mPreamble;
    std::map<ShaderNameTypePair, ShaderHandleTimestampPair> mShaders;
    std::map<std::vector<const ShaderNameTypePair*>, ProgramHandle> mPrograms;

public:
    ShaderSet() = default;

    // Destructor releases all owned shaders
    ~ShaderSet();

    // The version string to prepend to all shaders
    // Separated from the preamble because #version doesn't compile in C++
    void SetVersion(const std::string& version);

    // A string that gets prepended to every shader that gets compiled
    // Useful for compile-time constant #defines (like attrib locations)
    void SetPreamble(const std::string& preamble);

    // Convenience for reading the preamble from a file
    // The preamble is NOT auto-reloaded.
    void SetPreambleFile(const std::string& preambleFilename);

    // list of (file name, shader type) pairs
    // eg: AddProgram({ {"foo.vert", GL_VERTEX_SHADER}, {"bar.frag", GL_FRAGMENT_SHADER} });
    GLuint* AddProgram(const std::vector<std::pair<std::string, GLenum>>& typedShaders);

    // Polls the timestamps of all the shaders and recompiles/relinks them if they changed
    void UpdatePrograms();

    // Convenience to add shaders based on extension file naming conventions
    // vertex shader: .vert
    // fragment shader: .frag
    // geometry shader: .geom
    // tessellation control shader: .tesc
    // tessellation evaluation shader: .tese
    // compute shader: .comp
    // eg: AddProgramFromExts({"foo.vert", "bar.frag"});
    GLuint* AddProgramFromExts(const std::vector<std::string>& shaders);
};