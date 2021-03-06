/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/Shaders/Shader.h"
#include "Rendering/Shaders/ShaderHandler.h"
#include "Rendering/Shaders/LuaShaderContainer.h"
#include "Rendering/Shaders/GLSLCopyState.h"
#include "Rendering/GL/myGL.h"

#include "System/SafeUtil.h"
#include "System/StringUtil.h"
#include "System/FileSystem/FileHandler.h"
#include "System/Sync/HsiehHash.h"
#include "System/Log/ILog.h"

#include "System/Config/ConfigHandler.h"

#include <algorithm>
#include <array>
#ifdef DEBUG
	#include <string.h> // strncmp
#endif


/*****************************************************************/

#define LOG_SECTION_SHADER "Shader"
LOG_REGISTER_SECTION_GLOBAL(LOG_SECTION_SHADER)

// use the specific section for all LOG*() calls in this source file
#ifdef LOG_SECTION_CURRENT
	#undef LOG_SECTION_CURRENT
#endif
#define LOG_SECTION_CURRENT LOG_SECTION_SHADER

/*****************************************************************/

CONFIG(bool, UseShaderCache).defaultValue(true).description("If already compiled shaders should be shared via a cache.");


/*****************************************************************/

static bool glslIsValid(GLuint obj)
{
	assert(glIsShader(obj) || glIsProgram(obj));

	GLint status = 0;

	if (glIsShader(obj))
		glGetShaderiv(obj, GL_COMPILE_STATUS, &status);
	else
		glGetProgramiv(obj, GL_LINK_STATUS, &status);

	return (status != 0);
}


static std::string glslGetLog(GLuint obj)
{
	const bool isShader = glIsShader(obj);
	assert(glIsShader(obj) || glIsProgram(obj));

	int infologLength = 0;
	int maxLength = 0;

	if (isShader)
		glGetShaderiv(obj, GL_INFO_LOG_LENGTH, &maxLength);
	else
		glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &maxLength);

	std::string infoLog;
	infoLog.resize(maxLength);

	if (isShader)
		glGetShaderInfoLog(obj, maxLength, &infologLength, &infoLog[0]);
	else
		glGetProgramInfoLog(obj, maxLength, &infologLength, &infoLog[0]);

	infoLog.resize(infologLength);
	return infoLog;
}


static std::string GetShaderSource(const std::string& srcData)
{
	// if this is present, assume srcData is the source text
	if (srcData.find("void main()") != std::string::npos)
		return srcData;

	// otherwise assume srcData is the name of a file
	std::string soPath = "shaders/" + srcData;
	std::string soSource = "";

	CFileHandler soFile(soPath);

	if (soFile.FileExists()) {
		soSource.resize(soFile.FileSize());
		soFile.Read(&soSource[0], soFile.FileSize());
	} else {
		LOG_L(L_ERROR, "[%s] file \"%s\" not found", __func__, soPath.c_str());
	}

	return soSource;
}

static bool ExtractVersionDirective(std::string& src, std::string& version)
{
	const auto pos = src.find("#version ");

	if (pos != std::string::npos) {
		const auto eol = src.find('\n', pos) + 1;
		version = src.substr(pos, eol - pos);
		src.erase(pos, eol - pos);
		return true;
	}

	return false;
}

/*****************************************************************/


namespace Shader {
	static NullShaderObject nullShaderObject_(0, "");
	static NullProgramObject nullProgramObject_("NullProgram");

	NullShaderObject* nullShaderObject = &nullShaderObject_;
	NullProgramObject* nullProgramObject = &nullProgramObject_;


	/*****************************************************************/

	unsigned int IShaderObject::GetHash() const {
		unsigned int hash = 127;
		hash = HsiehHash((const void*)   srcText.data(),    srcText.size(), hash); // srcTextHash is not worth it, only called on reload
		hash = HsiehHash((const void*)modDefStrs.data(), modDefStrs.size(), hash);
		hash = HsiehHash((const void*)rawDefStrs.data(), rawDefStrs.size(), hash); // rawDefStrsHash is not worth it, only called on reload
		return hash;
	}


	bool IShaderObject::ReloadFromTextOrFile()
	{
		std::string newText = GetShaderSource(srcData);

		if (newText != srcText) {
			srcText = std::move(newText);
			return true;
		}

		return false;
	}




	/*****************************************************************/

	void IProgramObject::Release(bool deleteShaderObjs) {
		if (deleteShaderObjs) {
			// if false, do not assume these are heap-allocated
			for (IShaderObject*& so: shaderObjs) {
				delete so;
			}
		}

		glid = 0;
		hash = 0;

		valid = false;
		bound = false;

		uniformStates.clear();
		shaderObjs.clear();
		luaTextures.clear();
		log.clear();
	}


	bool IProgramObject::LoadFromLua(const std::string& filename) {
		return Shader::LoadFromLua(this, filename);
	}

	void IProgramObject::MaybeReload(bool validate)
	{
		// if no change to any flag, skip the (expensive) reload
		if (shaderFlags.HashSet() && !shaderFlags.Updated())
			return;

		Reload(!shaderFlags.HashSet(), validate);
		PrintDebugInfo();
	}

	void IProgramObject::PrintDebugInfo()
	{
		#if 0
		LOG_L(L_DEBUG, "Uniform States for program-object \"%s\":", name.c_str());
		LOG_L(L_DEBUG, "Defs:\n %s", (shaderFlags.GetString()).c_str());
		LOG_L(L_DEBUG, "Uniforms:");

		for (const auto& p : uniformStates) {
			const int uloc = GetUniformLocation(p.second.GetName());

			if (!p.second.IsInitialized()) {
				LOG_L(L_DEBUG, "\t%s: uninitialized (loc=%i)", (p.second.GetName()).c_str(), curUsed);
			} else {
				LOG_L(L_DEBUG, "\t%s: x=float:%f;int:%i y=%f z=%f used=%i", (p.second.GetName()).c_str(), p.second.GetFltValues()[0], p.second.GetIntValues()[0], p.second.GetFltValues()[1], p.second.GetFltValues()[2], curUsed);
			}
		}
		#endif
	}

	UniformState* IProgramObject::GetNewUniformState(const std::string name)
	{
		const size_t hash = hashString(name.c_str());
		const auto it = uniformStates.emplace(hash, name);

		UniformState* us = &(it.first->second);
		us->SetLocation(GetUniformLoc(name));

		return us;
	}


	void IProgramObject::AddTextureBinding(const int texUnit, const std::string& luaTexName)
	{
		LuaMatTexture luaTex;

		if (!LuaOpenGLUtils::ParseTextureImage(nullptr, luaTex, luaTexName))
			return;

		luaTextures[texUnit] = luaTex;
	}

	void IProgramObject::BindTextures() const
	{
		for (const auto& p: luaTextures) {
			glActiveTexture(GL_TEXTURE0 + p.first);
			(p.second).Bind();
		}
		glActiveTexture(GL_TEXTURE0);
	}




	/*****************************************************************/

	GLSLProgramObject::GLSLProgramObject(const std::string& poName): IProgramObject(poName) {
		glid = glCreateProgram();
	}

	void GLSLProgramObject::Enable() {
		MaybeReload(true);
		glUseProgram(glid);
		IProgramObject::Enable();
	}

	void GLSLProgramObject::Disable() {
		glUseProgram(0);
		IProgramObject::Disable();
	}


	bool GLSLProgramObject::CreateAndLink() {
		bool shadersValid = true;

		assert(glid == 0);

		if ((glid = glCreateProgram()) == 0)
			return false;

		for (IShaderObject*& so: shaderObjs) {
			// NOTE:
			//   cso will call glDeleteShader when it goes out of scope
			//   this is fine according to the GL docs and saves us from
			//   having to delete every attached shader when Release'ing
			//
			//   "If a shader object is deleted while it is attached to a program object, it will be
			//   flagged for deletion, and deletion will not occur until glDetachShader is called to
			//   detach it from all program objects to which it is attached."
			auto gso = static_cast<GLSLShaderObject*>(so);
			auto cso = std::move(gso->CreateAndCompileShaderObject(log));

			if (!cso.valid) {
				shadersValid = false;
				continue;
			}

			glAttachShader(glid, cso.id);
		}

		if (!shadersValid)
			return false;

		glLinkProgram(glid);

		// append the linker-log
		log.append(glslGetLog(glid));

		return (glslIsValid(glid));
	}

	bool GLSLProgramObject::ValidateAndCopyUniforms(unsigned int tgtProgID, unsigned int srcProgID, bool validate)
	{
		bool isValid = valid;

		if (validate)
			isValid = Validate();

		if (isValid) {
			// fill in uniformStates
			GLSLCopyState(tgtProgID, srcProgID, &uniformStates);
			return true;
		}

		if (!log.empty())
			LOG_L(L_WARNING, "[GLSL-PO::%s][validation-log (program-object=%s)]\n%s\n", __func__, name.c_str(), log.c_str());

		return false;
	}


	bool GLSLProgramObject::Validate() {
		GLint validated = 0;

		glValidateProgram(glid);
		glGetProgramiv(glid, GL_VALIDATE_STATUS, &validated);

		// append the validation-log
		log.append(glslGetLog(glid));

		#if 0
		// check if there are unset uniforms left
		GLsizei numUniforms = 0;
		GLsizei maxUniformNameLength = 0;

		glGetProgramiv(glid, GL_ACTIVE_UNIFORMS, &numUniforms);
		glGetProgramiv(glid, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxUniformNameLength);

		if (maxUniformNameLength <= 0)
			return valid;

		std::string bufname(maxUniformNameLength, 0);
		for (int i = 0; i < numUniforms; ++i) {
			GLsizei nameLength = 0;
			GLint size = 0;
			GLenum type = 0;
			glGetActiveUniform(glid, i, maxUniformNameLength, &nameLength, &size, &type, &bufname[0]);
			bufname[nameLength] = 0;

			if (nameLength == 0)
				continue;

			if (strncmp(&bufname[0], "gl_", 3) == 0)
				continue;

			if (uniformStates.find(hashString(&bufname[0])) != uniformStates.end())
				continue;

			LOG_L(L_WARNING, "[GLSL-PO::%s] program-object %s has unset uniform %s", __func__, name.c_str(), &bufname[0]);
		}
		#endif

		return (validated != 0);
	}


	void GLSLProgramObject::Release(bool deleteShaderObjs) {
		const unsigned int _glid = glid;

		IProgramObject::Release(deleteShaderObjs);
		glDeleteProgram(_glid);
		shaderFlags.Clear();
	}


	void GLSLProgramObject::Link() {
		MaybeReload(false);
		assert(glIsProgram(glid));
	}


	void GLSLProgramObject::Reload(bool force, bool validate) {
		const unsigned int _glid = glid;
		const unsigned int _hash = hash;

		const bool isValid = valid;
		const bool useCache = isValid && configHandler->GetBool("UseShaderCache");

		// early-exit in case of an empty program (TODO: glDeleteProgram it?)
		if (!ReloadState(force || !isValid || (_glid == 0))) {
			valid = false;
			return;
		}

		CShaderHandler::ShaderCache& shaderCache = shaderHandler->GetShaderCache();

		const auto& CachedProg = [&](unsigned int hc) { return ((useCache)? shaderCache.Find(hc): 0); };
		const auto& UpdateProg = [&](unsigned int hc) { return ((glid = CachedProg(hc)) != 0 || CreateAndLink()); };

		// recompile if post-reload <hash> has no entry in cache (id 0), validate on success
		// then add the pre-reload <_hash, _glid> program pair unless it already has an entry
		// NOTE:
		//   validation was done even if !valid before but used to fail on ATI
		//   (see springrts.com/mantis/view.php?id=4715); change "validate" to
		//   "validate && !globalRendering->haveATI" if this reoccurs
		// TODO: get rid of validation warnings forcing the "|| true"
		valid = (UpdateProg(hash) && (ValidateAndCopyUniforms(glid, _glid * isValid, validate) || true));

		if (useCache && shaderCache.Push(_hash, _glid))
			return;

		// cache was unused or already contained a program for <_hash>
		// (e.g. if reloading did not change the hash), so better hope
		// that (cache[_hash] == _glid) != glid
		if (hash == _hash)
			return;

		glDeleteProgram(_glid);
	}


	bool GLSLProgramObject::ReloadState(bool reloadShaderObjs) {
		log.clear();

		ClearUniformLocations();
		SetShaderDefinitions(shaderFlags.GetString());

		if (reloadShaderObjs)
			ReloadShaderObjects();

		RecalculateShaderHash();
		return (!shaderObjs.empty());
	}


	void GLSLProgramObject::ClearUniformLocations() {
		// clear all uniform locations
		for (auto& usPair: uniformStates) {
			usPair.second.SetLocation(GL_INVALID_INDEX);
		}
	}

	void GLSLProgramObject::SetShaderDefinitions(const std::string& defs) {
		// NOTE: this does not preserve the #version pragma
		for (IShaderObject*& so: shaderObjs) {
			so->SetDefinitions(defs);
		}
	}

	void GLSLProgramObject::ReloadShaderObjects() {
		// reload shaders from text or file
		for (IShaderObject*& so: shaderObjs) {
			so->ReloadFromTextOrFile();
		}
	}

	void GLSLProgramObject::RecalculateShaderHash() {
		// calculate shader hash from flags and source-text
		hash = shaderFlags.UpdateHash();

		for (const IShaderObject* so: shaderObjs) {
			hash ^= so->GetHash();
		}
	}



	int GLSLProgramObject::GetUniformType(const int idx) {
		GLint size = 0;
		GLenum type = 0;
		// NB: idx can not be a *location* returned by glGetUniformLoc except on Nvidia
		glGetActiveUniform(glid, idx, 0, nullptr, &size, &type, nullptr);
		assert(size == 1); // arrays aren't handled yet
		return type;
	}

	int GLSLProgramObject::GetUniformLoc(const std::string& name) {
		return glGetUniformLocation(glid, name.c_str());
	}

	void GLSLProgramObject::SetUniformLocation(const std::string& name) {
		uniformLocs.push_back(hashString(name.c_str()));
		GetUniformLocation(name);
	}


	void GLSLProgramObject::SetUniform(UniformState* uState, int   v0)                               { assert(IsBound()); if (uState->Set(v0            )) glUniform1i(uState->GetLocation(), v0             ); }
	void GLSLProgramObject::SetUniform(UniformState* uState, float v0)                               { assert(IsBound()); if (uState->Set(v0            )) glUniform1f(uState->GetLocation(), v0             ); }
	void GLSLProgramObject::SetUniform(UniformState* uState, int   v0, int   v1)                     { assert(IsBound()); if (uState->Set(v0, v1        )) glUniform2i(uState->GetLocation(), v0, v1         ); }
	void GLSLProgramObject::SetUniform(UniformState* uState, float v0, float v1)                     { assert(IsBound()); if (uState->Set(v0, v1        )) glUniform2f(uState->GetLocation(), v0, v1         ); }
	void GLSLProgramObject::SetUniform(UniformState* uState, int   v0, int   v1, int   v2)           { assert(IsBound()); if (uState->Set(v0, v1, v2    )) glUniform3i(uState->GetLocation(), v0, v1, v2     ); }
	void GLSLProgramObject::SetUniform(UniformState* uState, float v0, float v1, float v2)           { assert(IsBound()); if (uState->Set(v0, v1, v2    )) glUniform3f(uState->GetLocation(), v0, v1, v2     ); }
	void GLSLProgramObject::SetUniform(UniformState* uState, int   v0, int   v1, int   v2, int   v3) { assert(IsBound()); if (uState->Set(v0, v1, v2, v3)) glUniform4i(uState->GetLocation(), v0, v1, v2, v3 ); }
	void GLSLProgramObject::SetUniform(UniformState* uState, float v0, float v1, float v2, float v3) { assert(IsBound()); if (uState->Set(v0, v1, v2, v3)) glUniform4f(uState->GetLocation(), v0, v1, v2, v3 ); }

	void GLSLProgramObject::SetUniform2v(UniformState* uState, const int*   v) { assert(IsBound()); if (uState->Set2v(v)) glUniform2iv(uState->GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform2v(UniformState* uState, const float* v) { assert(IsBound()); if (uState->Set2v(v)) glUniform2fv(uState->GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform3v(UniformState* uState, const int*   v) { assert(IsBound()); if (uState->Set3v(v)) glUniform3iv(uState->GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform3v(UniformState* uState, const float* v) { assert(IsBound()); if (uState->Set3v(v)) glUniform3fv(uState->GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform4v(UniformState* uState, const int*   v) { assert(IsBound()); if (uState->Set4v(v)) glUniform4iv(uState->GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform4v(UniformState* uState, const float* v) { assert(IsBound()); if (uState->Set4v(v)) glUniform4fv(uState->GetLocation(), 1, v); }

	void GLSLProgramObject::SetUniformMatrix2x2(UniformState* uState, bool transp, const float* v) { assert(IsBound()); if (uState->Set2x2(v, transp)) glUniformMatrix2fv(uState->GetLocation(), 1, transp, v); }
	void GLSLProgramObject::SetUniformMatrix3x3(UniformState* uState, bool transp, const float* v) { assert(IsBound()); if (uState->Set3x3(v, transp)) glUniformMatrix3fv(uState->GetLocation(), 1, transp, v); }
	void GLSLProgramObject::SetUniformMatrix4x4(UniformState* uState, bool transp, const float* v) { assert(IsBound()); if (uState->Set4x4(v, transp)) glUniformMatrix4fv(uState->GetLocation(), 1, transp, v); }


	void GLSLProgramObject::SetUniform1i(int idx, int   v0                              ) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0            )) glUniform1i(it->second.GetLocation(), v0            ); }
	void GLSLProgramObject::SetUniform2i(int idx, int   v0, int   v1                    ) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0, v1        )) glUniform2i(it->second.GetLocation(), v0, v1        ); }
	void GLSLProgramObject::SetUniform3i(int idx, int   v0, int   v1, int   v2          ) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0, v1, v2    )) glUniform3i(it->second.GetLocation(), v0, v1, v2    ); }
	void GLSLProgramObject::SetUniform4i(int idx, int   v0, int   v1, int   v2, int   v3) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0, v1, v2, v3)) glUniform4i(it->second.GetLocation(), v0, v1, v2, v3); }
	void GLSLProgramObject::SetUniform1f(int idx, float v0                              ) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0            )) glUniform1f(it->second.GetLocation(), v0            ); }
	void GLSLProgramObject::SetUniform2f(int idx, float v0, float v1                    ) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0, v1        )) glUniform2f(it->second.GetLocation(), v0, v1        ); }
	void GLSLProgramObject::SetUniform3f(int idx, float v0, float v1, float v2          ) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0, v1, v2    )) glUniform3f(it->second.GetLocation(), v0, v1, v2    ); }
	void GLSLProgramObject::SetUniform4f(int idx, float v0, float v1, float v2, float v3) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0, v1, v2, v3)) glUniform4f(it->second.GetLocation(), v0, v1, v2, v3); }

	void GLSLProgramObject::SetUniform2iv(int idx, const int*   v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set2v(v)) glUniform2iv(it->second.GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform3iv(int idx, const int*   v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set3v(v)) glUniform3iv(it->second.GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform4iv(int idx, const int*   v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set4v(v)) glUniform4iv(it->second.GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform2fv(int idx, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set2v(v)) glUniform2fv(it->second.GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform3fv(int idx, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set3v(v)) glUniform3fv(it->second.GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform4fv(int idx, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set4v(v)) glUniform4fv(it->second.GetLocation(), 1, v); }

	void GLSLProgramObject::SetUniformMatrix2fv(int idx, bool transp, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set2x2(v, transp)) glUniformMatrix2fv(it->second.GetLocation(), 1, transp, v); }
	void GLSLProgramObject::SetUniformMatrix3fv(int idx, bool transp, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set3x3(v, transp)) glUniformMatrix3fv(it->second.GetLocation(), 1, transp, v); }
	void GLSLProgramObject::SetUniformMatrix4fv(int idx, bool transp, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set4x4(v, transp)) glUniformMatrix4fv(it->second.GetLocation(), 1, transp, v); }



	GLSLShaderObject::CompiledShaderObject::~CompiledShaderObject() { glDeleteShader(id); }
	GLSLShaderObject::CompiledShaderObject GLSLShaderObject::CreateAndCompileShaderObject(std::string& programLog)
	{
		CompiledShaderObject cso;
		// ReloadFromTextOrFile must have been called
		assert(!srcText.empty());

		std::string versionStr;
		std::string sourceStr = srcText;
		std::string defFlags  = rawDefStrs + "\n" + modDefStrs;

		// extract #version pragma and put it on the first line (only allowed there)
		// version pragma in definitions overrides version pragma in source (if any)
		ExtractVersionDirective(sourceStr, versionStr);
		ExtractVersionDirective(defFlags, versionStr);

		if (!versionStr.empty()) EnsureEndsWith(&versionStr, "\n");
		if (!defFlags.empty())   EnsureEndsWith(&defFlags,   "\n");

		std::array<const GLchar*, 7> sources = {
			"// SHADER VERSION\n",
			versionStr.c_str(),
			"// SHADER FLAGS\n",
			defFlags.c_str(),
			"// SHADER SOURCE\n",
			"#line 1\n",
			sourceStr.c_str()
		};

		glShaderSource(cso.id = glCreateShader(type), sources.size(), &sources[0], nullptr);
		glCompileShader(cso.id);

		if (!(cso.valid = glslIsValid(cso.id))) {
			const std::string& shaderLog = glslGetLog(cso.id);
			const std::string& shaderName = (srcData.find("void main()") != std::string::npos)? "unknown" : srcData;

			LOG_L(L_WARNING, "[GLSL-SO::%s] shader-object name: %s, compile-log:\n%s\n", __func__, shaderName.c_str(), shaderLog.c_str());
			LOG_L(L_WARNING, "\n%s%s%s%s%s%s%s", sources[0], sources[1], sources[2], sources[3], sources[4], sources[5], sources[6]);

			programLog.append(shaderLog);
		}

		return (std::move(cso));
	}
}

