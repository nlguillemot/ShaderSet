#include "shaderset.h"

#ifdef _WIN32
#include <Windows.h>
#else
// Not Windows? Assume unix-like.
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

// Replace with your own GL header include
#include "opengl.h"

#include <string>
#include <fstream>
#include <cstdio>

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

    timestamp = fileStat.st_mtimespec.tv_sec;
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
    for (std::pair<const ShaderNameTypePair, ShaderHandleTimestampPair>& shader : mShaders)
    {
        glDeleteShader(shader.second.first);
    }

    for (std::pair<const std::vector<const ShaderNameTypePair*>, ProgramHandle>& program : mPrograms)
    {
        glDeleteProgram(program.second);
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
        
    for (const std::pair<std::string, GLenum>& shaderNameType : typedShaders)
    {
        auto foundShader = mShaders.emplace(shaderNameType, ShaderHandleTimestampPair{}).first;
        if (!foundShader->second.first)
        {
            foundShader->second.first = glCreateShader(shaderNameType.second);
        }
        shaderNameTypes.push_back(&foundShader->first);
    }

    auto foundProgram = mPrograms.emplace(shaderNameTypes, 0).first;
    if (!foundProgram->second)
    {
        foundProgram->second = glCreateProgram();
        for (const ShaderNameTypePair* shader : shaderNameTypes)
        {
            glAttachShader(foundProgram->second, mShaders[*shader].first);
        }
    }
    return &foundProgram->second;
}

void ShaderSet::UpdatePrograms()
{
    // find all shaders with updated timestamps
    std::set<std::pair<const ShaderNameTypePair, ShaderHandleTimestampPair>*> updatedShaders;
    for (std::pair<const ShaderNameTypePair, ShaderHandleTimestampPair>& shader : mShaders)
    {
        uint64_t timestamp = GetShaderFileTimestamp(shader.first.first.c_str());
        if (timestamp > shader.second.second)
        {
            shader.second.second = timestamp;
            updatedShaders.insert(&shader);
        }
    }

    // recompile all updated shaders
    for (std::pair<const ShaderNameTypePair, ShaderHandleTimestampPair>* shader : updatedShaders)
    {
        std::string version = "#version " + mVersion + "\n";
        std::string premable = mPreamble + "\n";
        std::string source = ShaderStringFromFile(shader->first.first.c_str()) + "\n";

        const char* strings[] = {
            version.c_str(),
            premable.c_str(),
            source.c_str()
        };
        GLint lengths[] = {
            (GLint)version.length(),
            (GLint)premable.length(),
            (GLint)source.length()
        };

        glShaderSource(shader->second.first, sizeof(strings) / sizeof(*strings), strings, lengths);
        glCompileShader(shader->second.first);
            
        GLint status;
        glGetShaderiv(shader->second.first, GL_COMPILE_STATUS, &status);
        if (!status)
        {
            GLint logLength;
            glGetShaderiv(shader->second.first, GL_INFO_LOG_LENGTH, &logLength);
            std::vector<char> log(logLength + 1);
            glGetShaderInfoLog(shader->second.first, logLength, NULL, log.data());
            fprintf(stderr, "Error compiling %s: %s\n", shader->first.first.c_str(), log.data());
        }
    }

    // relink all programs that had their shaders updated
    for (std::pair<const std::vector<const ShaderNameTypePair*>, ProgramHandle>& program : mPrograms)
    {
        bool programNeedsRelink = false;
        for (const ShaderNameTypePair* programShader : program.first)
        {
            for (std::pair<const ShaderNameTypePair, ShaderHandleTimestampPair>* shader : updatedShaders)
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

        if (programNeedsRelink)
        {
            glLinkProgram(program.second);

            GLint status;
            glGetProgramiv(program.second, GL_LINK_STATUS, &status);
            if (!status)
            {
                GLint logLength;
                glGetProgramiv(program.second, GL_INFO_LOG_LENGTH, &logLength);
                std::vector<char> log(logLength + 1);
                glGetProgramInfoLog(program.second, logLength, NULL, log.data());
                    
                fprintf(stderr, "Error linking program (");
                for (const ShaderNameTypePair* shader : program.first)
                {
                    if (&shader != &program.first.front())
                    {
                        fprintf(stderr, ", ");
                    }

                    fprintf(stderr, "%s", shader->first.c_str());
                }
                fprintf(stderr, "): %s\n", log.data());
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