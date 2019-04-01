#include "coremin.h"
#include "math/math.h"
#include "gldrv/gldrv.h"

#include <SDL.h>

#if SGL_BUILD_RELEASE
	#define LOG(...)
#else
	#define LOG(...) printf(__VA_ARGS__)
#endif

Malloc * gMalloc = nullptr;

/// Screen variables
point2 fboSize;

/// Time variables
float32 dt;
float32 currTime;
uint64 currTick;
uint64 prevTick;

/// Camera variables
vec3 cameraLocation;
vec3 cameraVelocity;
quat cameraRotation;
mat4 cameraTransform;
mat4 projectionMatrix;

union VertexVec3
{
	float32 array[3];

	struct
	{
		float32 x, y, z;
	};

	struct
	{
		float32 r, g, b;
	};
};

struct VertexColor
{
	uint32 data;

	struct
	{
		ubyte r, g, b, a;
	};
};

struct VertexData
{
	VertexVec3 pos;
	VertexVec3 norm;
	VertexColor color;
};

class FileReader
{
protected:
	FILE * fp;
	void * buffer;
	uint64 len;

public:
	FileReader(const char * filename) :
		fp(fopen(filename, "r"))
	{
		if (fp)
		{
			// Get file length
			fseek(fp, 0, SEEK_END);
			len = ftell(fp);
			fseek(fp, 0, SEEK_SET);

			// Allocate buffer
			if (posix_memalign((void**)&buffer, sizeof(void*), len + 1) == 0)
			{
				get<char>()[len] = 0;
				fread(buffer, 1, len, fp);
			}
		}
	}

	~FileReader()
	{
		if (fp) fclose(fp);
		if (buffer) free(buffer);
	}

	FORCE_INLINE bool isValid() const { return fp && buffer && len > 0; }

	template<typename T>
	FORCE_INLINE T * get() { return reinterpret_cast<T*>(buffer); }
};

class ShaderProgram
{
protected:
	/// Program name
	uint32 name;

	/// Uniforms map
	Map<String, uint32> uniforms;

public:
	/// Default constructor
	FORCE_INLINE ShaderProgram() :
		name(glCreateProgram()),
		uniforms{} {}
	
	/// Set active
	FORCE_INLINE void bind()
	{
		glUseProgram(name);
	}

public:
	/// Set shader
	FORCE_INLINE void setShader(uint32 shader)
	{
		glAttachShader(name, shader);
	}

	/// Link program
	FORCE_INLINE void link()
	{
		glLinkProgram(name);
	}

	/// Get program status
	FORCE_INLINE int32 getStatus(uint32 iv = GL_LINK_STATUS)
	{
		int32 status = 0;
		glGetProgramiv(name, iv, &status);
		return status;
	}

protected:
	/// Set uniform
	template<typename T, typename Lambda>
	void setUniform_internal(const String & key, T val, Lambda glFun)
	{
		auto it = uniforms.find(key);
		if (it != uniforms.end())
			glFun(it->second, val);
		else
		{
			uint32 slot = glGetUniformLocation(name, *key);
			glFun(slot, val);

			// Cache in map
			uniforms.insert(key, slot);
		}
	}

public:
	/// Set uniform
	template<typename T>
	void setUniform(const String & key, T val);
};

template<>
FORCE_INLINE void ShaderProgram::setUniform<float32>(const String & key, float32 val)
{
	setUniform_internal(key, val, glUniform1f);
}

template<>
FORCE_INLINE void ShaderProgram::setUniform<int32>(const String & key, int32 val)
{
	setUniform_internal(key, val, glUniform1i);
}

template<>
FORCE_INLINE void ShaderProgram::setUniform<uint32>(const String & key, uint32 val)
{
	setUniform_internal(key, val, glUniform1ui);
}

template<>
FORCE_INLINE void ShaderProgram::setUniform<Vec2<int32>>(const String & key, Vec2<int32> val)
{
	setUniform_internal(key, val, [](uint32 slot, Vec2<int32> val) {

		glUniform2iv(slot, 1, val.buffer);
	});
}

template<>
FORCE_INLINE void ShaderProgram::setUniform<const Vec3<float32, true>&>(const String & key, const Vec3<float32, true> & val)
{
	setUniform_internal(key, val, [](uint32 slot, const Vec3<float32, true> & val) {

		glUniform3fv(slot, 1, val.buffer);
	});
}

template<>
FORCE_INLINE void ShaderProgram::setUniform<const Vec3<float32, false>&>(const String & key, const Vec3<float32, false> & val)
{
	setUniform_internal(key, val, [](uint32 slot, const Vec3<float32, false> & val) {

		glUniform3fv(slot, 1, val.buffer);
	});
}

template<>
FORCE_INLINE void ShaderProgram::setUniform<const mat4&>(const String & key, const mat4 & val)
{
	setUniform_internal(key, val, [](uint32 slot, const mat4 & val) {

		glUniformMatrix4fv(slot, 1, GL_TRUE, val.array);
	});
}

void setupPerlin()
{
	int32 perms[0x100];
	Vec3<float32, false> grads[0x100];
	const float32 freq = 2.f * M_PI / (float32)0x100;

	// Fill uniformly
	for (uint32 i = 0; i < 0x100; ++i)
		perms[i] = i;
	
	// Shuffle
	for (uint32 i = 0, range = 0x100; i < 0x100; ++i, --range)
	{
		uint8 k = rand() % range + i;
		swap(perms[i], perms[k]);
	}

	// Generate gradients
	for (uint32 i = 0; i < 0x100; ++i)
		grads[i] = Vec3<float32, false>(
			Math::cos(perms[i] * freq),
			Math::cos(perms[perms[i]] * freq),
			Math::sin(perms[i] * freq)
		).normalize();
	
	// Upload to GPU
	sizet offset;
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (offset = 0x100 * sizeof(perms[0])), perms);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, offset, 0x100 * sizeof(grads[0]), grads);
};

int32 main()
{
	Memory::createGMalloc();
	srand(clock());

	Map<uint32, float32> keys;

	initOpenGL();

	fboSize = point2(2560, 1440);

	SDL_Window * window = SDL_CreateWindow("light", 0, 0, fboSize.x, fboSize.y, SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS);
	SDL_GLContext context = SDL_GL_CreateContext(window);
	SDL_GL_SetSwapInterval(0);

	glEnable(GL_DEPTH_TEST);

	//////////////////////////////////////////////////
	// Program setup
	//////////////////////////////////////////////////
	
	ShaderProgram drawProg;
	uint32 drawShader = glCreateShader(GL_COMPUTE_SHADER);

	{
		FileReader source = "src/light/shaders/volume/.comp";
		const char * buffer = source.get<char>();
		glShaderSource(drawShader, 1, &buffer, nullptr);
		glCompileShader(drawShader);
		drawProg.setShader(drawShader);

		int32 status = 0;
		glGetShaderiv(drawShader, GL_COMPILE_STATUS, &status);
		if (status == GL_FALSE) printf("shader not compiled\n");
	}
	
	drawProg.link();
	drawProg.bind();
	if (drawProg.getStatus() == 0) printf("draw program not linked correctly\n");
	
	ShaderProgram genProg;
	uint32 cShader = glCreateShader(GL_COMPUTE_SHADER);

	{
		FileReader source = "src/light/shaders/generation/.comp";
		const char * buffer = source.get<char>();
		glShaderSource(cShader, 1, &buffer, nullptr);
		glCompileShader(cShader);
		genProg.setShader(cShader);

		int32 status = 0;
		glGetShaderiv(cShader, GL_COMPILE_STATUS, &status);
		if (status == GL_FALSE) printf("shader not compiled\n");
	}
	
	genProg.link();
	genProg.bind();
	if (genProg.getStatus() == 0) printf("generation program not linked correctly\n");

	// Setup perlin noise tables
	uint32 perlinTables;
	const sizet perlinTableSize = 0x100 * (sizeof(int32) + sizeof(Vec3<float32, false>));
	glGenBuffers(1, &perlinTables);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, perlinTables);
	glBufferData(GL_SHADER_STORAGE_BUFFER, perlinTableSize, nullptr, GL_STATIC_DRAW);
	setupPerlin();

	uint32 drawFbo;
	glGenFramebuffers(1, &drawFbo);
	glBindFramebuffer(GL_FRAMEBUFFER, drawFbo);

	uint32 colorBuffer;
	glGenTextures(1, &colorBuffer);
	glBindTexture(GL_TEXTURE_2D, colorBuffer);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, fboSize.x, fboSize.y, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorBuffer, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	uint32 volumeData;
	glGenTextures(1, &volumeData);
	glBindTexture(GL_TEXTURE_3D, volumeData);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F, 256, 256, 256, 0, GL_RED, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	/// Generate volume data
	LOG("generating volume data ...\n");
	genProg.bind();
	glBindImageTexture(0, volumeData, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, perlinTables);
	glDispatchCompute(256 / 8, 256 / 8, 256 / 8);
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	glBindTexture(GL_TEXTURE_3D, volumeData);
	//glGenerateMipMap(GL_TEXTURE_3D);
	LOG("volume data generated ...\n");

	//////////////////////////////////////////////////
	// Camera setup
	//////////////////////////////////////////////////
	
	projectionMatrix = mat4::glProjection(M_PI_2, 0.5f);
	cameraLocation = vec3(0.f, 0.f, -5.f);
	cameraVelocity = vec3::zero;
	cameraRotation = quat(0.f, vec3::up);

	//////////////////////////////////////////////////
	// Main loop
	//////////////////////////////////////////////////

	bool bRunning = true;
	while (bRunning)
	{
		// Update time variables
		currTime += (dt = ((currTick = SDL_GetPerformanceCounter()) - prevTick) / (float32)SDL_GetPerformanceFrequency());
		prevTick = currTick;

		printf("%f s -> %f fps\n", dt, 1.f / dt);

		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			switch (e.type)
			{
				case SDL_QUIT:
					bRunning = false;
					break;
				
				case SDL_KEYDOWN:
					keys[e.key.keysym.sym] = 1.f;
					bRunning &= e.key.keysym.sym != SDLK_ESCAPE;
					break;
				
				case SDL_KEYUP:
					keys[e.key.keysym.sym] = 0.f;
					break;
			}
		}

		//////////////////////////////////////////////////
		// Camera position and rotation
		//////////////////////////////////////////////////

		const float32 cameraSpeed = 4.f;
		const float32 cameraBrake = 2.f;
		vec3 cameraAcceleration = cameraRotation * vec3(
			keys[SDLK_d] - keys[SDLK_a],
			keys[SDLK_SPACE] - keys[SDLK_LCTRL],
			keys[SDLK_w] - keys[SDLK_s]
		) * cameraSpeed - (cameraVelocity) * cameraBrake;
		cameraVelocity += cameraAcceleration * dt;
		cameraLocation += cameraVelocity * dt;

		cameraRotation
			= quat((keys[SDLK_RIGHT] - keys[SDLK_LEFT]) * dt, cameraRotation.up())
			* quat((keys[SDLK_LEFT] - keys[SDLK_RIGHT]) * dt, cameraRotation.forward())
			* quat((keys[SDLK_DOWN] - keys[SDLK_UP]) * dt, cameraRotation.right())
			* cameraRotation;

		cameraTransform = mat4::rotation(!cameraRotation) * mat4::translation(-cameraLocation);
		const mat4 viewMatrix = projectionMatrix * cameraTransform;

		//////////////////////////////////////////////////
		// Draw
		//////////////////////////////////////////////////
		
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		const point2 hFboSize = fboSize;

		drawProg.bind();
		drawProg.setUniform<float32>("time", currTime);
		drawProg.setUniform<point2>("fboSize", hFboSize);
		drawProg.setUniform<float32>("samplingStep", 0.5f);
		drawProg.setUniform<const mat4&>("viewMatrix", viewMatrix);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_3D, volumeData);
		glBindImageTexture(0, colorBuffer, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
		glDispatchCompute(hFboSize.x / 32, hFboSize.y / 32, 1);

		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

		// Test generation by blitting to viewport
		glBindFramebuffer(GL_READ_FRAMEBUFFER, drawFbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glBlitFramebuffer(0, 0, hFboSize.x, hFboSize.y, 0, 0, fboSize.x, fboSize.y, GL_COLOR_BUFFER_BIT, GL_LINEAR);

		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		SDL_GL_SwapWindow(window);
	}

	return 0;
}