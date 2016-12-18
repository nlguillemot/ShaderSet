#include "shaderset.h"

#ifdef _WIN32
#include <Windows.h>
#else
// Not Windows? Assume unix-like.
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <string>
#include <fstream>
#include <cstdio>
#include <algorithm>

static uint64_t GetShaderFileTimestamp(const char* filename)
{
    uint64_t timestamp = 0;

#ifdef _WIN32
    int filenameBufferSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename, -1, NULL, 0);
    if (filenameBufferSize == 0)
    {
        return 0;
    }

    WCHAR* wfilename = new WCHAR[filenameBufferSize];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename, -1, wfilename, filenameBufferSize))
    {
        // Potential improvement: How can we poll the timestamp without sometimes blocking write access from other programs? (OS race condition)
        HANDLE hFile = CreateFileW(wfilename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            FILETIME lastWriteTime;
            if (GetFileTime(hFile, NULL, NULL, &lastWriteTime))
            {
                LARGE_INTEGER largeWriteTime;
                largeWriteTime.HighPart = lastWriteTime.dwHighDateTime;
                largeWriteTime.LowPart = lastWriteTime.dwLowDateTime;
                timestamp = largeWriteTime.QuadPart;
            }
            CloseHandle(hFile);
        }
    }
    delete[] wfilename;
#else
    struct stat fileStat;

    if (stat(filename, &fileStat) == -1)
    {
        perror(filename);
        return 0;
    }

#ifdef __APPLE__
    timestamp = fileStat.st_mtimespec.tv_sec;
#else
    timestamp = fileStat.st_mtime;
#endif
#endif

    return timestamp;
}

static std::string ShaderStringFromFile(const char* filename)
{
    std::ifstream fs(filename);
    if (!fs)
    {
        return "";
    }

    std::string s(
        std::istreambuf_iterator<char>{fs},
        std::istreambuf_iterator<char>{});

    return s;
}

ShaderSet::~ShaderSet()
{
    for (std::pair<const ShaderNameTypePair, Shader>& shader : mShaders)
    {
        glDeleteShader(shader.second.Handle);
    }

    for (std::pair<const std::vector<const ShaderNameTypePair*>, Program>& program : mPrograms)
    {
        glDeleteProgram(program.second.InternalHandle);
    }
}

void ShaderSet::SetVersion(const std::string& version)
{
    mVersion = version;
}

void ShaderSet::SetPreamble(const std::string& preamble)
{
    mPreamble = preamble;
}

GLuint* ShaderSet::AddProgram(const std::vector<std::pair<std::string, GLenum>>& typedShaders)
{
    std::vector<const ShaderNameTypePair*> shaderNameTypes;
        
    // find references to existing shaders, and create ones that didn't exist previously.
    for (const std::pair<std::string, GLenum>& shaderNameType : typedShaders)
    {
        ShaderNameTypePair tmpShaderNameType;
        std::tie(tmpShaderNameType.Name, tmpShaderNameType.Type) = shaderNameType;

        auto foundShader = mShaders.emplace(std::move(tmpShaderNameType), Shader{}).first;
        if (!foundShader->second.Handle)
        {
            foundShader->second.Handle = glCreateShader(shaderNameType.second);
            // The sign bit is masked out, since some shader compilers treat the #line as signed, and others treat it unsigned.
            foundShader->second.HashName = (int32_t)std::hash<std::string>()(shaderNameType.first) & 0x7FFFFFFF;
        }
        shaderNameTypes.push_back(&foundShader->first);
    }

    // ensure the programs have a canonical order
    std::sort(begin(shaderNameTypes), end(shaderNameTypes));
    shaderNameTypes.erase(std::unique(begin(shaderNameTypes), end(shaderNameTypes)), end(shaderNameTypes));

    // find the program associated to these shaders (or create it if missing)
    auto foundProgram = mPrograms.emplace(shaderNameTypes, Program{}).first;
    if (!foundProgram->second.InternalHandle)
    {
        // public handle is 0 until the program has linked without error
        foundProgram->second.PublicHandle = 0;

        foundProgram->second.InternalHandle = glCreateProgram();
        for (const ShaderNameTypePair* shader : shaderNameTypes)
        {
            glAttachShader(foundProgram->second.InternalHandle, mShaders[*shader].Handle);
        }
    }

    return &foundProgram->second.PublicHandle;
}

void ShaderSet::UpdatePrograms()
{
    // find all shaders with updated timestamps
    std::set<std::pair<const ShaderNameTypePair, Shader>*> updatedShaders;
    for (std::pair<const ShaderNameTypePair, Shader>& shader : mShaders)
    {
        uint64_t timestamp = GetShaderFileTimestamp(shader.first.Name.c_str());
        if (timestamp > shader.second.Timestamp)
        {
            shader.second.Timestamp = timestamp;
            updatedShaders.insert(&shader);
        }
    }

    // recompile all updated shaders
    for (std::pair<const ShaderNameTypePair, Shader>* shader : updatedShaders)
    {
        // the #line prefix ensures error messages have the right line number for their file
        // the #line directive also allows specifying a "file name" number, which makes it possible to identify which file the error came from.
        std::string version = "#version " + mVersion + "\n";
        
        std::string defines;
        switch (shader->first.Type) {
        case GL_VERTEX_SHADER:          defines += "#define VERTEX_SHADER\n";             break;
        case GL_FRAGMENT_SHADER:        defines += "#define FRAGMENT_SHADER\n";           break;
        case GL_GEOMETRY_SHADER:        defines += "#define GEOMETRY_SHADER\n";           break;
        case GL_TESS_CONTROL_SHADER:    defines += "#define TESS_CONTROL_SHADER\n";       break;
        case GL_TESS_EVALUATION_SHADER: defines += "#define TESS_EVALUATION_SHADER\n";    break;
        case GL_COMPUTE_SHADER:         defines += "#define COMPUTE_SHADER\n";            break;
        }

        std::string preamble_hash = std::to_string((int32_t)std::hash<std::string>()("preamble") & 0x7FFFFFFF);
        std::string preamble = "#line 1 " + preamble_hash + "\n" + 
                               mPreamble + "\n";
        
        std::string source_hash = std::to_string(shader->second.HashName);
        std::string source = "#line 1 " + source_hash + "\n" + 
                             ShaderStringFromFile(shader->first.Name.c_str()) + "\n";

        const char* strings[] = {
            version.c_str(),
            defines.c_str(),
            preamble.c_str(),
            source.c_str()
        };
        GLint lengths[] = {
            (GLint)version.length(),
            (GLint)defines.length(),
            (GLint)preamble.length(),
            (GLint)source.length()
        };

        glShaderSource(shader->second.Handle, sizeof(strings) / sizeof(*strings), strings, lengths);
        glCompileShader(shader->second.Handle);
            
        GLint status;
        glGetShaderiv(shader->second.Handle, GL_COMPILE_STATUS, &status);
        if (!status)
        {
            GLint logLength;
            glGetShaderiv(shader->second.Handle, GL_INFO_LOG_LENGTH, &logLength);
            std::vector<char> log(logLength + 1);
            glGetShaderInfoLog(shader->second.Handle, logLength, NULL, log.data());
            
            std::string log_s = log.data();
            
            // replace all filename hashes in the error messages with actual filenames
            for (size_t found_preamble; (found_preamble = log_s.find(preamble_hash)) != std::string::npos;) {
                log_s.replace(found_preamble, preamble_hash.size(), "preamble");
            }
            for (size_t found_source; (found_source = log_s.find(source_hash)) != std::string::npos;) {
                log_s.replace(found_source, source_hash.size(), shader->first.Name);
            }

            fprintf(stderr, "Error compiling %s:\n%s\n", shader->first.Name.c_str(), log_s.c_str());
        }
    }

    // relink all programs that had their shaders updated and have all their shaders compiling successfully
    for (std::pair<const std::vector<const ShaderNameTypePair*>, Program>& program : mPrograms)
    {
        bool programNeedsRelink = false;
        for (const ShaderNameTypePair* programShader : program.first)
        {
            for (std::pair<const ShaderNameTypePair, Shader>* shader : updatedShaders)
            {
                if (&shader->first == programShader)
                {
                    programNeedsRelink = true;
                    break;
                }
            }

            if (programNeedsRelink)
                break;
        }

        // Don't attempt to link shaders that didn't compile successfully
        bool canRelink = true;
        if (programNeedsRelink)
        {
            for (const ShaderNameTypePair* programShader : program.first)
            {
                GLint status;
                glGetShaderiv(mShaders[*programShader].Handle, GL_COMPILE_STATUS, &status);
                if (!status)
                {
                    canRelink = false;
                    break;
                }
            }
        }

        if (programNeedsRelink && canRelink)
        {
            glLinkProgram(program.second.InternalHandle);

            GLint logLength;
            glGetProgramiv(program.second.InternalHandle, GL_INFO_LOG_LENGTH, &logLength);
            std::vector<char> log(logLength + 1);
            glGetProgramInfoLog(program.second.InternalHandle, logLength, NULL, log.data());

            std::string log_s = log.data();

            // replace all filename hashes in the error messages with actual filenames
            std::string preamble_hash = std::to_string((int32_t)std::hash<std::string>()("preamble"));
            for (size_t found_preamble; (found_preamble = log_s.find(preamble_hash)) != std::string::npos;) {
                log_s.replace(found_preamble, preamble_hash.size(), "preamble");
            }
            for (const ShaderNameTypePair* shaderInProgram : program.first)
            {
                std::string source_hash = std::to_string(mShaders[*shaderInProgram].HashName);
                for (size_t found_source; (found_source = log_s.find(source_hash)) != std::string::npos;) {
                    log_s.replace(found_source, source_hash.size(), shaderInProgram->Name);
                }
            }

            GLint status;
            glGetProgramiv(program.second.InternalHandle, GL_LINK_STATUS, &status);

            if (!status)
            {
                fprintf(stderr, "Error linking");
            }
            else
            {
                fprintf(stderr, "Successfully linked");
            }

            fprintf(stderr, " program (");
            for (const ShaderNameTypePair* shader : program.first)
            {
                if (shader != program.first.front())
                {
                    fprintf(stderr, ", ");
                }

                fprintf(stderr, "%s", shader->Name.c_str());
            }
            fprintf(stderr, ")");
            if (log[0] != '\0')
            {
                fprintf(stderr, ":\n%s\n", log_s.c_str());
            }
            else
            {
                fprintf(stderr, "\n");
            }

            if (!status)
            {
                program.second.PublicHandle = 0;
            }
            else
            {
                program.second.PublicHandle = program.second.InternalHandle;
            }
        }
    }
}

void ShaderSet::SetPreambleFile(const std::string& preambleFilename)
{
    SetPreamble(ShaderStringFromFile(preambleFilename.c_str()));
}

GLuint* ShaderSet::AddProgramFromExts(const std::vector<std::string>& shaders)
{
    std::vector<std::pair<std::string, GLenum>> typedShaders;
    for (const std::string& shader : shaders)
    {
        size_t extLoc = shader.find_last_of('.');
        if (extLoc == std::string::npos)
        {
            return nullptr;
        }

        GLenum shaderType;

        std::string ext = shader.substr(extLoc + 1);
        if (ext == "vert")
            shaderType = GL_VERTEX_SHADER;
        else if (ext == "frag")
            shaderType = GL_FRAGMENT_SHADER;
        else if (ext == "geom")
            shaderType = GL_GEOMETRY_SHADER;
        else if (ext == "tesc")
            shaderType = GL_TESS_CONTROL_SHADER;
        else if (ext == "tese")
            shaderType = GL_TESS_EVALUATION_SHADER;
        else if (ext == "comp")
            shaderType = GL_COMPUTE_SHADER;
        else
            return nullptr;

        typedShaders.emplace_back(shader, shaderType);
    }

    return AddProgram(typedShaders);
}

GLuint* ShaderSet::AddProgramFromCombinedFile(const std::string &filename, const std::vector<GLenum> &shaderTypes)
{
    std::vector<std::pair<std::string, GLenum>> typedShaders;

    for (auto type: shaderTypes)
        typedShaders.emplace_back(filename, type);

    return AddProgram(typedShaders);
}

