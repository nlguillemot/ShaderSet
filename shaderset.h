#pragma once

// Replace with your own GL header include
#include "opengl.h"

#include <vector>
#include <utility>
#include <map>
#include <set>

class ShaderSet
{
    // typedefs for readability
    using ShaderHandle = GLuint;
    using ProgramHandle = GLuint;

    // filename and shader type
    struct ShaderNameTypePair
    {
        std::string Name;
        GLenum Type;
        bool operator<(const ShaderNameTypePair& rhs) const { return std::tie(Name, Type) < std::tie(rhs.Name, rhs.Type); }
    };
    // shader's handle and the timestamp of the most recent update
    struct ShaderHandleTimestampPair
    {
        ShaderHandle Handle;
        uint64_t Timestamp;
    };
    // The handle exposed externally ("public") and the most recent (succeeding/failed) linked program ("internal")
    // the public handle becomes 0 when a linking failure happens, until the linking error gets fixed
    struct ProgramPublicInternalPair
    {
        ProgramHandle PublicHandle;
        ProgramHandle InternalHandle;
    };

    // the version in the version string that gets prepended to each shader
    std::string mVersion;
    // the preamble which gets prepended to each shader (for eg. shared binding conventions)
    std::string mPreamble;
    // maps shader name/types to handles, in order to reuse shared shaders.
    std::map<ShaderNameTypePair, ShaderHandleTimestampPair> mShaders;
    // allows looking up the program that represents a linked set of shaders
    std::map<std::vector<const ShaderNameTypePair*>, ProgramPublicInternalPair> mPrograms;

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