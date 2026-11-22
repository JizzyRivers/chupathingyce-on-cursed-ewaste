/*
GLES2_LAYER.C

The Steam Link's OpenGL ES 2 (a Vivante GC1000) filled out to the ES 3 the
renderer uses (chupathingyce's port/linux/src/d3d8_gl.c with HALO_GL_ES:
HALO_CONSOLE_GLES2, console_gles2_layer). gl_functions_load has loaded what
the driver has; this replaces the entry points ES 2 lacks, or has with less,
in the renderer's own pointers (halo_gl*, gl.h):

- vertex array objects, kept here and applied to ES 2's one set of
  attributes when one is bound
- sampler objects, applied to the textures bound to each unit
- separate read and draw framebuffers, kept here; a blit is a textured quad;
  a depth and stencil attachment is the two of ES 2's packed one
- the sized texture formats, as ES 2's unsized ones, and texture swizzles,
  applied to the texels as they are uploaded (ES 2 has none)
- the shaders: GLSL ES 3.00 source, as the renderer writes it, rewritten as
  GLSL ES 1.00 (attribute and varying, gl_FragColor, texture() as overloads
  of texture2D and textureCube, the vertex shader's attribute locations
  bound before the link)
- what the box has no use for (queries, 3D textures, buffer copies) does
  nothing, saying so once in the log

and the Android host's GL helpers the ES path calls (host_gl_*: xgpu.h).
*/

#include "platform.h"
#include "gl.h"
#include "xgpu.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ES 2's own names for what ES 3 has under its own (GLES2/gl2ext.h) */
#define LAYER_BGRA_EXT 0x80E1
#define LAYER_DEPTH_STENCIL_OES 0x84F9
#define LAYER_UNSIGNED_INT_24_8_OES 0x84FA
#define LAYER_DEPTH24_STENCIL8_OES 0x88F0
#define LAYER_RGBA8_OES 0x8058
/* (desktop GL's: the renderer's overlay sets all four at once) */
#ifndef GL_TEXTURE_SWIZZLE_RGBA
#define GL_TEXTURE_SWIZZLE_RGBA 0x8E46
#endif

#define LAYER_TEXTURE_UNITS 16
#define LAYER_ATTRIBUTES 16
#define LAYER_TRACKED 8192

/* ---------- the driver's own entry points, before they are replaced */

static struct
{
	__typeof__(halo_glGetIntegerv) GetIntegerv;
	__typeof__(halo_glActiveTexture) ActiveTexture;
	__typeof__(halo_glBindTexture) BindTexture;
	__typeof__(halo_glTexParameteri) TexParameteri;
	__typeof__(halo_glTexParameteriv) TexParameteriv;
	__typeof__(halo_glTexParameterf) TexParameterf;
	__typeof__(halo_glTexParameterfv) TexParameterfv;
	__typeof__(halo_glTexImage2D) TexImage2D;
	__typeof__(halo_glTexSubImage2D) TexSubImage2D;
	__typeof__(halo_glCompressedTexImage2D) CompressedTexImage2D;
	__typeof__(halo_glBindFramebuffer) BindFramebuffer;
	__typeof__(halo_glFramebufferTexture2D) FramebufferTexture2D;
	__typeof__(halo_glFramebufferRenderbuffer) FramebufferRenderbuffer;
	__typeof__(halo_glCheckFramebufferStatus) CheckFramebufferStatus;
	__typeof__(halo_glReadPixels) ReadPixels;
	__typeof__(halo_glBindBuffer) BindBuffer;
	__typeof__(halo_glEnableVertexAttribArray) EnableVertexAttribArray;
	__typeof__(halo_glDisableVertexAttribArray) DisableVertexAttribArray;
	__typeof__(halo_glVertexAttribPointer) VertexAttribPointer;
	__typeof__(halo_glCreateShader) CreateShader;
	__typeof__(halo_glShaderSource) ShaderSource;
	__typeof__(halo_glCompileShader) CompileShader;
	__typeof__(halo_glAttachShader) AttachShader;
	__typeof__(halo_glLinkProgram) LinkProgram;
	__typeof__(halo_glUseProgram) UseProgram;
	void (GL_APIENTRY *RenderbufferStorage)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
	void (GL_APIENTRY *GetFramebufferAttachmentParameteriv)(GLenum target, GLenum attachment, GLenum pname,
		GLint *params);
	void (GL_APIENTRY *VertexAttrib4f)(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
	void (GL_APIENTRY *DiscardFramebufferEXT)(GLenum target, GLsizei count, const GLenum *attachments);
} real;

/* ---------- the log: each kind of thing the layer cannot do, once */

static void layer_once(int *said, const char *format, ...)
{
	char line[256];
	va_list arguments;

	if (*said)
		return;
	*said = 1;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	platform_log("gles2: %s", line);
}

#define LAYER_ONCE(...) do { static int said; layer_once(&said, __VA_ARGS__); } while (0)

/* the driver calls the layer makes, counted for the frame timings' line
(each 300 frames: where the driver's time goes) */
static struct
{
	unsigned long draws, texture_parameters, texture_binds, uniforms, programs, pointers, framebuffers, links, compiles, gpu_indexed;
	/* draws with vertices the driver copies (client arrays), their vertices' bytes, and indexed draws' indices */
	unsigned long client_draws, client_bytes, index_count;
} counts;

/* (every draw since the start, for HALO_PERF_AB and the frame timing log,
steamlink_perf_log.c) */
static unsigned long draws_total;

unsigned long console_gles2_draw_count(void)
{
	return draws_total;
}

/* HALO_GLES2_TRACE=n: the first n framebuffer, texture, blit and draw calls
in the log, each draw finished (glFinish) and its error read before the
next: the last line before a GPU hang names the call that made it */
static long trace_left = -1;
static int profiling;
static void profile_report(void);

static int tracing(void)
{
	if (trace_left < 0)
	{
		const char *value = getenv("HALO_GLES2_TRACE");

		trace_left = value ? atol(value) : 0;
	}
	return trace_left > 0;
}

static void trace(const char *format, ...)
{
	char line[256];
	va_list arguments;

	if (!tracing())
		return;
	trace_left--;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	platform_log("gles2 trace: %s", line);
}

/* ---------- textures and samplers */

struct layer_texture
{
	GLsizei width, height;
	/* counts its uploads (a copy of it is made again after one) */
	unsigned long generation;
	/* level 0's format as the driver has it (compressed: the DXT format) */
	GLenum format;
	GLint max_level;
	/* the swizzle the renderer asked for: GL_RED...GL_ALPHA, GL_ZERO, GL_ONE */
	GLint swizzle[4];
	/* the minification filter asked for, and what was last given the driver
	(0: not yet) */
	GLint min_filter;
	GLint own_mag, own_wrap_s, own_wrap_t;
	GLint applied_min, applied_mag, applied_wrap_s, applied_wrap_t;
};

struct layer_sampler
{
	int used;
	GLint min_filter, mag_filter, wrap_s, wrap_t;
	/* D3DTADDRESS_BORDER on each axis, and its color: ES 2 has no border
	clamp, so the pixel shaders do it (layer_sampleN, border_uniforms_apply) */
	int border_s, border_t;
	GLfloat border_color[4];
};

static struct layer_texture textures[LAYER_TRACKED];
static struct layer_sampler samplers[LAYER_TRACKED];
/* each program's border uniforms (border_helpers_head), their locations
plus 1 (0 where it has none), and what was last given them */
static struct
{
	GLint color, axes;
	int uploaded;
	GLfloat values[2][4][4];
} border_programs[LAYER_TRACKED];
static void border_uniforms_apply(void);
static void position_uniforms_apply(void);
/* a program linked by vertex_pruned_link with its position's conversion
taken out of its vertex shader: the two vectors it is now, worked out from
the values the shader keeps of what they come from (position_uniforms_apply) */
struct position_program
{
	GLint scale, offset;
	/* viewport_scale, viewport_offset, c_58, c_59, screen_offset */
	GLint from[5];
	GLfloat last[17];
	int given;
};
static struct position_program *position_programs[LAYER_TRACKED];

static GLuint sampler_next = 1;
static GLuint active_unit;
/* the textures bound to each unit: [0] 2D, [1] cube */
static GLuint unit_textures[LAYER_TEXTURE_UNITS][2];

/* HALO_GLES2_MERGE_STATS: what each draw changes from the one before, for how
many draws could be merged (each costs the driver about half a millisecond
whatever its size). Blend, depth and the rest of the render state are not
looked at, so "nothing" and the others are the most that could merge. A
uniform counts only when its value changes. */
static struct
{
	unsigned long nothing, pointers, uniforms, textures, programs, framebuffers, uniform_writes, uniform_repeats;
} merge_stats;
static int merge_stats_on = -1;
static int merge_changed_pointers, merge_changed_uniforms, merge_changed_framebuffer;
static GLuint merge_program, merge_textures[LAYER_TEXTURE_UNITS][2];
static GLfloat (*merge_constants[LAYER_TRACKED])[4];

static void merge_stats_draw(GLuint program)
{
	int textures_changed;

	if (merge_stats_on < 0)
		merge_stats_on = getenv("HALO_GLES2_MERGE_STATS") != NULL;
	if (!merge_stats_on)
		return;
	textures_changed = memcmp(merge_textures, unit_textures, sizeof(unit_textures)) != 0;
	if (merge_changed_framebuffer)
		merge_stats.framebuffers++;
	else if (program != merge_program)
		merge_stats.programs++;
	else if (textures_changed)
		merge_stats.textures++;
	else if (merge_changed_uniforms)
		merge_stats.uniforms++;
	else if (merge_changed_pointers)
		merge_stats.pointers++;
	else
		merge_stats.nothing++;
	merge_program = program;
	memcpy(merge_textures, unit_textures, sizeof(unit_textures));
	merge_changed_pointers = merge_changed_uniforms = merge_changed_framebuffer = 0;
}

/* the constants of a program being written, against what it has */
static void merge_stats_uniform(GLuint program, GLint first, GLsizei count, const GLfloat *values)
{
	GLint index;

	if (merge_stats_on <= 0 || program >= LAYER_TRACKED)
		return;
	if (!merge_constants[program])
		merge_constants[program] = calloc(192, sizeof(GLfloat[4]));
	if (!merge_constants[program])
		return;
	for (index = 0; index < count && first + index < 192; index++)
	{
		merge_stats.uniform_writes++;
		if (memcmp(merge_constants[program][first + index], values + index * 4, sizeof(GLfloat[4])))
		{
			memcpy(merge_constants[program][first + index], values + index * 4, sizeof(GLfloat[4]));
			merge_changed_uniforms = 1;
		}
		else
			merge_stats.uniform_repeats++;
	}
}
static GLuint unit_samplers[LAYER_TEXTURE_UNITS];

static struct layer_texture *texture_get(GLuint texture)
{
	return texture && texture < LAYER_TRACKED ? &textures[texture] : NULL;
}

static int target_slot(GLenum target)
{
	return target == GL_TEXTURE_CUBE_MAP ? 1 : 0;
}

/* the texture an image target writes (a cube map's face is the cube's) */
static struct layer_texture *bound_texture(GLenum target)
{
	GLenum binding = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ?
		GL_TEXTURE_CUBE_MAP : target;

	if (active_unit >= LAYER_TEXTURE_UNITS || (binding != GL_TEXTURE_2D && binding != GL_TEXTURE_CUBE_MAP))
		return NULL;
	return texture_get(unit_textures[active_unit][target_slot(binding)]);
}

static void texture_reset(struct layer_texture *texture)
{
	memset(texture, 0, sizeof(*texture));
	texture->max_level = 1000;
	texture->swizzle[0] = GL_RED;
	texture->swizzle[1] = GL_GREEN;
	texture->swizzle[2] = GL_BLUE;
	texture->swizzle[3] = GL_ALPHA;
	texture->min_filter = GL_NEAREST_MIPMAP_LINEAR;
}

/* a mipmapped filter on a texture whose levels stop short of 1x1 leaves it
incomplete in ES 2 (black), where ES 3's GL_TEXTURE_MAX_LEVEL made it whole:
there the filter reads the first level only */
/* the texture's levels stop short of 1x1 (GL_TEXTURE_MAX_LEVEL): ES 3
draws such a texture whole, ES 2 has it incomplete, and Vivante's GPU hangs
sampling one, whatever its filter. It is given level 0 only (texture_level_kept) */
static int short_chain(const struct layer_texture *texture)
{
	GLsizei size = texture->width > texture->height ? texture->width : texture->height;
	GLint levels = 1;

	while (size > 1)
	{
		size >>= 1;
		levels++;
	}
	return texture->max_level + 1 < levels;
}

/* whether an upload of this level reaches the driver */
static int texture_level_kept(const struct layer_texture *texture, GLint level)
{
	if (!level || !texture || !texture->width)
		return 1;
	if (short_chain(texture))
	{
		LAYER_ONCE("textures whose levels stop short of 1x1 keep level 0 only");
		return 0;
	}
	return 1;
}

static GLint complete_min_filter(const struct layer_texture *texture, GLint filter)
{
	static int no_mipmaps = -1;

	/* (HALO_GLES2_NO_MIPMAPS: none sampled, to tell what hangs the GPU) */
	if (no_mipmaps < 0)
		no_mipmaps = getenv("HALO_GLES2_NO_MIPMAPS") != NULL;
	if (filter == GL_NEAREST || filter == GL_LINEAR || (!no_mipmaps && !short_chain(texture)))
		return filter;
	return filter == GL_NEAREST_MIPMAP_NEAREST || filter == GL_NEAREST_MIPMAP_LINEAR ? GL_NEAREST : GL_LINEAR;
}

static int is_texture_parameter_es3(GLenum pname)
{
	switch (pname)
	{
	case GL_TEXTURE_BASE_LEVEL:
	case GL_TEXTURE_MAX_LEVEL:
	case GL_TEXTURE_MIN_LOD:
	case GL_TEXTURE_MAX_LOD:
	case GL_TEXTURE_WRAP_R:
	case GL_TEXTURE_COMPARE_MODE:
	case GL_TEXTURE_COMPARE_FUNC:
	case GL_TEXTURE_SWIZZLE_R:
	case GL_TEXTURE_SWIZZLE_G:
	case GL_TEXTURE_SWIZZLE_B:
	case GL_TEXTURE_SWIZZLE_A:
	case GL_TEXTURE_SWIZZLE_RGBA:
	case GL_TEXTURE_BORDER_COLOR:
	case 0x84FE: /* GL_TEXTURE_MAX_ANISOTROPY_EXT: the box has none */
		return 1;
	default:
		return 0;
	}
}

static GLint wrap_mode(GLint mode)
{
	/* (no border clamp in ES 2) */
	return mode == GL_CLAMP_TO_BORDER ? GL_CLAMP_TO_EDGE : mode;
}

/* the sampler bound to unit, given to the texture bound there (on ES 2 a
texture holds its own filter and wrap) */
/* ES 2 has no samplers: a sampler's parameters are put on the textures bound
where it is bound. With none bound, the texture's own go back (ES 3 has a
sampler override them only while it is bound): without that, a texture bound
for a moment on a unit with a sampler keeps the sampler's filter, and one
with no smaller levels (the overlay's text atlas, uploaded to wherever the
game left its units) is then incomplete, and samples black. 0: never set */
/* (HALO_GLES2_EXPERIMENT's bits, below: 256, no constants uploaded; 512, no
sampler state set; 1024, no texture bound. For what each costs on a frozen
picture, HALO_PERF_AB's layer_experiment) */
static unsigned int experiment_bits;

static void sampler_apply(GLuint unit)
{
	struct layer_sampler *sampler = NULL;
	int slot;

	if (experiment_bits & 512)
		return;
	if (unit >= LAYER_TEXTURE_UNITS)
		return;
	if (unit_samplers[unit] && unit_samplers[unit] < LAYER_TRACKED)
		sampler = &samplers[unit_samplers[unit]];
	for (slot = 0; slot < 2; slot++)
	{
		GLenum target = slot ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
		struct layer_texture *texture = texture_get(unit_textures[unit][slot]);
		GLint min_filter, mag_filter, wrap_s, wrap_t;

		if (!texture)
			continue;
		min_filter = sampler ? sampler->min_filter : texture->min_filter;
		mag_filter = sampler ? sampler->mag_filter : texture->own_mag;
		wrap_s = sampler ? sampler->wrap_s : texture->own_wrap_s;
		wrap_t = sampler ? sampler->wrap_t : texture->own_wrap_t;
		min_filter = min_filter ? complete_min_filter(texture, min_filter) : texture->applied_min;
		if (!mag_filter)
			mag_filter = texture->applied_mag;
		if (!wrap_s)
			wrap_s = texture->applied_wrap_s;
		if (!wrap_t)
			wrap_t = texture->applied_wrap_t;
		if (texture->applied_min == min_filter && texture->applied_mag == mag_filter &&
			texture->applied_wrap_s == wrap_s && texture->applied_wrap_t == wrap_t)
		{
			continue;
		}
		if (unit != active_unit)
			real.ActiveTexture(GL_TEXTURE0 + unit);
		counts.texture_parameters++;
		if (texture->applied_min != min_filter)
			real.TexParameteri(target, GL_TEXTURE_MIN_FILTER, min_filter);
		if (texture->applied_mag != mag_filter)
			real.TexParameteri(target, GL_TEXTURE_MAG_FILTER, mag_filter);
		if (texture->applied_wrap_s != wrap_s)
			real.TexParameteri(target, GL_TEXTURE_WRAP_S, wrap_s);
		if (texture->applied_wrap_t != wrap_t)
			real.TexParameteri(target, GL_TEXTURE_WRAP_T, wrap_t);
		if (unit != active_unit)
			real.ActiveTexture(GL_TEXTURE0 + active_unit);
		texture->applied_min = min_filter;
		texture->applied_mag = mag_filter;
		texture->applied_wrap_s = wrap_s;
		texture->applied_wrap_t = wrap_t;
	}
}

static void GL_APIENTRY layer_ActiveTexture(GLenum unit)
{
	active_unit = unit - GL_TEXTURE0;
	real.ActiveTexture(unit);
}

/* a 1x1 white texture where the renderer binds none: a shader that samples
a unit with nothing bound makes Vivante's driver print "texture(id=0) is
not completed" (to standard error, the game's log) for each draw, which
was most of the box's log and a write to its flash each time */
static GLuint texture_white;

static void GL_APIENTRY layer_BindTexture(GLenum target, GLuint texture)
{
	counts.texture_binds++;
	if (!(experiment_bits & 1024))
		real.BindTexture(target, texture || target != GL_TEXTURE_2D ? texture : texture_white);
	if (active_unit < LAYER_TEXTURE_UNITS && (target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP))
	{
		unit_textures[active_unit][target_slot(target)] = texture;
		sampler_apply(active_unit);
	}
	else if (target != GL_TEXTURE_2D && target != GL_TEXTURE_CUBE_MAP)
	{
		LAYER_ONCE("no texture target 0x%x (3D and array textures are not drawn)", (unsigned)target);
	}
}

static void GL_APIENTRY layer_DeleteTextures_hook(GLsizei count, const GLuint *names);
static __typeof__(halo_glDeleteTextures) real_DeleteTextures;
static __typeof__(halo_glGenTextures) real_GenTextures;

static void GL_APIENTRY layer_GenTextures(GLsizei count, GLuint *names)
{
	GLsizei index;

	real_GenTextures(count, names);
	for (index = 0; index < count; index++)
	{
		struct layer_texture *texture = texture_get(names[index]);

		if (texture)
			texture_reset(texture);
	}
}

static void GL_APIENTRY layer_DeleteTextures_hook(GLsizei count, const GLuint *names)
{
	GLsizei index;
	int unit;

	for (index = 0; index < count; index++)
	{
		for (unit = 0; unit < LAYER_TEXTURE_UNITS; unit++)
		{
			if (unit_textures[unit][0] == names[index])
				unit_textures[unit][0] = 0;
			if (unit_textures[unit][1] == names[index])
				unit_textures[unit][1] = 0;
		}
	}
	real_DeleteTextures(count, names);
}

static void GL_APIENTRY layer_TexParameteri(GLenum target, GLenum pname, GLint value)
{
	struct layer_texture *texture = bound_texture(target);

	switch (pname)
	{
	case GL_TEXTURE_SWIZZLE_R:
	case GL_TEXTURE_SWIZZLE_G:
	case GL_TEXTURE_SWIZZLE_B:
	case GL_TEXTURE_SWIZZLE_A:
		if (texture)
			texture->swizzle[pname - GL_TEXTURE_SWIZZLE_R] = value;
		return;
	case GL_TEXTURE_MAX_LEVEL:
		if (texture)
		{
			texture->max_level = value;
			texture->applied_min = 0;
		}
		return;
	case GL_TEXTURE_MIN_FILTER:
		if (texture)
		{
			texture->min_filter = value;
			value = complete_min_filter(texture, value);
			texture->applied_min = value;
		}
		break;
	case GL_TEXTURE_MAG_FILTER:
		if (texture)
			texture->applied_mag = texture->own_mag = value;
		break;
	case GL_TEXTURE_WRAP_S:
		value = wrap_mode(value);
		if (texture)
			texture->applied_wrap_s = texture->own_wrap_s = value;
		break;
	case GL_TEXTURE_WRAP_T:
		value = wrap_mode(value);
		if (texture)
			texture->applied_wrap_t = texture->own_wrap_t = value;
		break;
	default:
		if (is_texture_parameter_es3(pname))
			return;
		break;
	}
	real.TexParameteri(target, pname, value);
}

static void GL_APIENTRY layer_TexParameteriv(GLenum target, GLenum pname, const GLint *values)
{
	struct layer_texture *texture = bound_texture(target);

	if (pname == GL_TEXTURE_SWIZZLE_RGBA)
	{
		if (texture)
			memcpy(texture->swizzle, values, sizeof(texture->swizzle));
		return;
	}
	layer_TexParameteri(target, pname, values[0]);
}

static void GL_APIENTRY layer_TexParameterf(GLenum target, GLenum pname, GLfloat value)
{
	if (is_texture_parameter_es3(pname))
		return;
	real.TexParameterf(target, pname, value);
}

static void GL_APIENTRY layer_TexParameterfv(GLenum target, GLenum pname, const GLfloat *values)
{
	if (is_texture_parameter_es3(pname))
		return;
	real.TexParameterfv(target, pname, values);
}

static void GL_APIENTRY layer_GenSamplers(GLsizei count, GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		GLuint name = sampler_next < LAYER_TRACKED ? sampler_next++ : 0;

		if (name)
		{
			samplers[name].used = 1;
			samplers[name].min_filter = GL_NEAREST_MIPMAP_LINEAR;
			samplers[name].mag_filter = GL_LINEAR;
			samplers[name].wrap_s = GL_REPEAT;
			samplers[name].wrap_t = GL_REPEAT;
		}
		else
		{
			LAYER_ONCE("out of sampler objects");
		}
		names[index] = name;
	}
}

static void GL_APIENTRY layer_BindSampler(GLuint unit, GLuint sampler)
{
	if (unit >= LAYER_TEXTURE_UNITS)
		return;
	unit_samplers[unit] = sampler < LAYER_TRACKED ? sampler : 0;
	sampler_apply(unit);
}

static void sampler_parameter(GLuint name, GLenum pname, GLint value)
{
	struct layer_sampler *sampler;
	GLuint unit;

	if (!name || name >= LAYER_TRACKED)
		return;
	sampler = &samplers[name];
	switch (pname)
	{
	case GL_TEXTURE_MIN_FILTER: sampler->min_filter = value; break;
	case GL_TEXTURE_MAG_FILTER: sampler->mag_filter = value; break;
	case GL_TEXTURE_WRAP_S: sampler->wrap_s = wrap_mode(value); sampler->border_s = value == GL_CLAMP_TO_BORDER; break;
	case GL_TEXTURE_WRAP_T: sampler->wrap_t = wrap_mode(value); sampler->border_t = value == GL_CLAMP_TO_BORDER; break;
	default: return;
	}
	for (unit = 0; unit < LAYER_TEXTURE_UNITS; unit++)
	{
		if (unit_samplers[unit] == name)
			sampler_apply(unit);
	}
}

static void GL_APIENTRY layer_SamplerParameteri(GLuint sampler, GLenum pname, GLint value)
{
	sampler_parameter(sampler, pname, value);
}

static void GL_APIENTRY layer_SamplerParameterf(GLuint sampler, GLenum pname, GLfloat value)
{
	sampler_parameter(sampler, pname, (GLint)value);
}

static void GL_APIENTRY layer_SamplerParameterfv(GLuint sampler, GLenum pname, const GLfloat *values)
{
	if (pname == GL_TEXTURE_BORDER_COLOR)
	{
		if (sampler && sampler < LAYER_TRACKED)
			memcpy(samplers[sampler].border_color, values, sizeof(samplers[sampler].border_color));
		return;
	}
	sampler_parameter(sampler, pname, (GLint)values[0]);
}

/* ---------- texture images */

static int swizzle_identity(const GLint swizzle[4])
{
	return swizzle[0] == GL_RED && swizzle[1] == GL_GREEN && swizzle[2] == GL_BLUE && swizzle[3] == GL_ALPHA;
}

/* RGBA texels with the texture's swizzle applied, in a buffer the caller
frees; NULL where none is needed */
static unsigned char *swizzled_texels(const struct layer_texture *texture, GLenum format, GLenum type,
	GLsizei width, GLsizei height, const void *pixels)
{
	const unsigned char *source = pixels;
	unsigned char *result, *destination;
	size_t count, index;
	int channel;

	if (!texture || !pixels || swizzle_identity(texture->swizzle))
		return NULL;
	if (format != GL_RGBA || type != GL_UNSIGNED_BYTE)
	{
		LAYER_ONCE("no swizzle for format 0x%x type 0x%x", (unsigned)format, (unsigned)type);
		return NULL;
	}
	count = (size_t)width * (size_t)height;
	result = malloc(count * 4);
	if (!result)
		return NULL;
	destination = result;
	for (index = 0; index < count; index++, source += 4, destination += 4)
	{
		for (channel = 0; channel < 4; channel++)
		{
			switch (texture->swizzle[channel])
			{
			case GL_RED: destination[channel] = source[0]; break;
			case GL_GREEN: destination[channel] = source[1]; break;
			case GL_BLUE: destination[channel] = source[2]; break;
			case GL_ALPHA: destination[channel] = source[3]; break;
			case GL_ZERO: destination[channel] = 0; break;
			default: destination[channel] = 255; break;
			}
		}
	}
	return result;
}

/* ES 3's sized internal format as ES 2's format and type (ES 2's internal
format is its format) */
static int image_format(GLint *internal, GLenum *format, GLenum *type)
{
	switch (*internal)
	{
	case GL_RGBA8:
	case GL_RGBA:
		*internal = GL_RGBA;
		*format = GL_RGBA;
		return 1;
	case GL_RGB8:
	case GL_RGB:
		*internal = GL_RGB;
		*format = GL_RGB;
		return 1;
	case GL_RGB565:
		*internal = GL_RGB;
		*format = GL_RGB;
		*type = GL_UNSIGNED_SHORT_5_6_5;
		return 1;
	case GL_RGBA4:
	case GL_RGB5_A1:
		*internal = GL_RGBA;
		*format = GL_RGBA;
		return 1;
	case GL_R8:
	case GL_RED:
		/* (sampled as red, green and blue all three: the overlay reads red) */
		*internal = GL_LUMINANCE;
		*format = GL_LUMINANCE;
		return 1;
	case GL_RG8:
	case GL_RG:
		*internal = GL_LUMINANCE_ALPHA;
		*format = GL_LUMINANCE_ALPHA;
		return 1;
	case GL_DEPTH24_STENCIL8:
	case GL_DEPTH_STENCIL:
		*internal = LAYER_DEPTH_STENCIL_OES;
		*format = LAYER_DEPTH_STENCIL_OES;
		*type = LAYER_UNSIGNED_INT_24_8_OES;
		return 1;
	case GL_DEPTH_COMPONENT24:
	case GL_DEPTH_COMPONENT32F:
	case GL_DEPTH_COMPONENT16:
	case GL_DEPTH_COMPONENT:
		*internal = GL_DEPTH_COMPONENT;
		*format = GL_DEPTH_COMPONENT;
		*type = GL_UNSIGNED_INT;
		return 1;
	default:
		LAYER_ONCE("no internal format 0x%x", (unsigned)*internal);
		return 0;
	}
}

static void GL_APIENTRY layer_TexImage2D(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height,
	GLint border, GLenum format, GLenum type, const void *pixels)
{
	struct layer_texture *texture = bound_texture(target);
	unsigned char *swizzled;

	if (!image_format(&internal, &format, &type))
		return;
	if (!texture_level_kept(texture, level))
		return;
	if (texture && level == 0)
	{
		texture->width = width;
		texture->height = height;
		texture->format = (GLenum)internal;
		texture->applied_min = 0;
	}
	swizzled = swizzled_texels(texture, format, type, width, height, pixels);
	if (texture)
		texture->generation++;
	if (level == 0)
		trace("teximage target 0x%x %dx%d internal 0x%x format 0x%x type 0x%x%s", (unsigned)target, (int)width,
			(int)height, (unsigned)internal, (unsigned)format, (unsigned)type, pixels ? "" : " (empty)");
	real.TexImage2D(target, level, internal, width, height, border, format, type, swizzled ? swizzled : pixels);
	free(swizzled);
}

static void GL_APIENTRY layer_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width,
	GLsizei height, GLenum format, GLenum type, const void *pixels)
{
	struct layer_texture *texture = bound_texture(target);
	unsigned char *swizzled;

	if (!texture_level_kept(texture, level))
		return;
	if (texture)
		texture->generation++;
	if (format == GL_RED)
		format = GL_LUMINANCE;
	else if (format == GL_RG)
		format = GL_LUMINANCE_ALPHA;
	swizzled = swizzled_texels(texture, format, type, width, height, pixels);
	real.TexSubImage2D(target, level, x, y, width, height, format, type, swizzled ? swizzled : pixels);
	free(swizzled);
}

static void GL_APIENTRY layer_CompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei width,
	GLsizei height, GLint border, GLsizei size, const void *data)
{
	struct layer_texture *texture = bound_texture(target);

	if (!texture_level_kept(texture, level))
		return;
	if (texture && level == 0)
	{
		texture->width = width;
		texture->height = height;
		texture->format = internal;
		texture->applied_min = 0;
	}
	if (texture)
		texture->generation++;
	if (texture && !swizzle_identity(texture->swizzle))
		LAYER_ONCE("a compressed texture's swizzle is not applied");
	real.CompressedTexImage2D(target, level, internal, width, height, border, size, data);
}

static void GL_APIENTRY layer_TexImage3D(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height,
	GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels)
{
	(void)target; (void)level; (void)internal; (void)width; (void)height; (void)depth; (void)border;
	(void)format; (void)type; (void)pixels;
	LAYER_ONCE("no 3D or array textures");
}

static void GL_APIENTRY layer_CompressedTexImage3D(GLenum target, GLint level, GLenum internal, GLsizei width,
	GLsizei height, GLsizei depth, GLint border, GLsizei size, const void *data)
{
	(void)target; (void)level; (void)internal; (void)width; (void)height; (void)depth; (void)border;
	(void)size; (void)data;
	LAYER_ONCE("no 3D or array textures");
}

/* ---------- buffers, in the process's memory

The renderer streams its vertices and indices into a ring of large buffers
(16 MB and 2 MB, d3d8_gl.c), a few hundred writes a frame; Vivante's ES 2
flushes the CPU's caches over the buffer for each, which made a frame take
seconds. Here a buffer object is memory of the process's, and a draw hands
the driver pointers into it (ES 2's client arrays), which copies only what
the draw reads. Only the mirror's buffers (console_gl_buffer_alias) have a
GPU copy, its vertices and indices drawn from that. */

static __typeof__(halo_glGenBuffers) real_GenBuffers;
static __typeof__(halo_glBufferData) real_BufferData;
static __typeof__(halo_glBufferSubData) real_BufferSubData;

struct layer_buffer
{
	unsigned char *data;
	/* the data is someone else's memory (console_gl_buffer_alias) */
	int aliased;
	/* a GPU buffer with it, which draws read (the mirror's segments: drawn
	again every frame, which as client arrays the driver copied each time) */
	GLuint gpu;
	/* -1 for a name made with nothing in it yet */
	GLsizeiptr size;
	/* an aliased buffer's: each 4 KB page's upload_generation at its latest
	upload (the packed normals' copies: normals_stream) */
	unsigned int *pages;
};

/* counts the aliased buffers' uploads */
static unsigned int upload_generation = 1;

static void buffer_pages_uploaded(struct layer_buffer *buffer, GLintptr offset, GLsizeiptr size)
{
	GLintptr page;

	if (!buffer->pages || size <= 0)
		return;
	upload_generation++;
	for (page = offset >> 12; page <= (offset + size - 1) >> 12; page++)
		buffer->pages[page] = upload_generation;
}

/* whether bytes start to end of an aliased buffer are as they were at
generation since */
static int buffer_pages_unchanged(const struct layer_buffer *buffer, unsigned long start, unsigned long end,
	unsigned int since)
{
	unsigned long page;

	if (!buffer->pages || end <= start || end > (unsigned long)buffer->size)
		return 0;
	for (page = start >> 12; page <= (end - 1) >> 12; page++)
	{
		if (buffer->pages[page] > since)
			return 0;
	}
	return 1;
}

static struct layer_buffer buffers[LAYER_TRACKED];
static GLuint buffer_next = 1;
/* GL_COPY_READ_BUFFER's and GL_COPY_WRITE_BUFFER's (the mirror's copies) */
static GLuint copy_read_buffer, copy_write_buffer;

static struct layer_buffer *buffer_get(GLuint name)
{
	return name && name < LAYER_TRACKED ? &buffers[name] : NULL;
}

/* where an offset into a buffer is in memory (a client pointer, with no
buffer) */
static const void *buffer_pointer(GLuint name, const void *offset)
{
	struct layer_buffer *buffer = buffer_get(name);

	if (!buffer)
		return offset;
	return buffer->data ? buffer->data + (uintptr_t)offset : NULL;
}

static void GL_APIENTRY layer_GenBuffers(GLsizei count, GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		while (buffer_next < LAYER_TRACKED && buffers[buffer_next].size != 0)
			buffer_next++;
		if (buffer_next >= LAYER_TRACKED)
		{
			LAYER_ONCE("out of buffer objects");
			names[index] = 0;
			continue;
		}
		buffers[buffer_next].size = -1;
		names[index] = buffer_next++;
	}
}

static void GL_APIENTRY layer_DeleteBuffers(GLsizei count, const GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		struct layer_buffer *buffer = buffer_get(names[index]);

		if (!buffer)
			continue;
		if (!buffer->aliased)
			free(buffer->data);
		free(buffer->pages);
		buffer->pages = NULL;
		buffer->data = NULL;
		buffer->aliased = 0;
		buffer->size = 0;
		if (names[index] < buffer_next)
			buffer_next = names[index];
	}
}

static GLuint *buffer_binding(GLenum target);

static void GL_APIENTRY layer_BufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	GLuint *binding = buffer_binding(target);
	struct layer_buffer *buffer = binding ? buffer_get(*binding) : NULL;
	unsigned char *memory;

	(void)usage;
	if (!buffer)
		return;
	/* (the same size again is the renderer orphaning it: kept, as the
	driver has copied what the draws before it read) */
	if (buffer->data && buffer->size == size)
	{
		if (data)
			memcpy(buffer->data, data, (size_t)size);
		return;
	}
	/* (malloc's untouched pages cost nothing: a 16 MB ring buffer is as much
	of the box's memory as is streamed into it) */
	memory = size > 0 ? malloc((size_t)size) : NULL;
	if (size > 0 && !memory)
	{
		LAYER_ONCE("no memory for a buffer of %ld bytes", (long)size);
		return;
	}
	free(buffer->data);
	buffer->data = memory;
	buffer->size = size;
	if (data && memory)
		memcpy(memory, data, (size_t)size);
}

static void GL_APIENTRY layer_BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	GLuint *binding = buffer_binding(target);
	struct layer_buffer *buffer = binding ? buffer_get(*binding) : NULL;

	if (!buffer || !buffer->data || offset < 0 || size < 0 || offset + size > buffer->size)
		return;
	/* (an aliased buffer's upload from its own memory: already there) */
	if (buffer->data + offset != data)
		memcpy(buffer->data + offset, data, (size_t)size);
	buffer_pages_uploaded(buffer, offset, size);
	if (buffer->gpu)
	{
		real.BindBuffer(GL_ARRAY_BUFFER, buffer->gpu);
		real_BufferSubData(GL_ARRAY_BUFFER, offset, size, buffer->data + offset);
		real.BindBuffer(GL_ARRAY_BUFFER, 0);
	}
}

/* HALO_CONSOLE_GLES2's (d3d8_gl.c's mirror of the Xbox window): the buffer
bound to target is that memory itself, as client arrays read it */
void console_gl_buffer_alias(GLenum target, void *memory, unsigned long size)
{
	GLuint *binding = buffer_binding(target);
	struct layer_buffer *buffer = binding ? buffer_get(*binding) : NULL;

	if (!buffer)
		return;
	if (!buffer->aliased)
		free(buffer->data);
	buffer->data = memory;
	buffer->size = (GLsizeiptr)size;
	buffer->aliased = 1;
	free(buffer->pages);
	buffer->pages = calloc(size / 4096 + 1, sizeof(*buffer->pages));
	/* (and a GPU buffer of it, empty: the renderer uploads each page before
	the first draw from it and as the game changes it, layer_BufferSubData;
	much of the window is not memory at all, and reading it faults) */
	if (!buffer->gpu)
		real_GenBuffers(1, &buffer->gpu);
	if (buffer->gpu)
	{
		real.BindBuffer(GL_ARRAY_BUFFER, buffer->gpu);
		real_BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)size, NULL, GL_STATIC_DRAW);
		real.BindBuffer(GL_ARRAY_BUFFER, 0);
	}
}

/* ---------- vertex arrays */

struct layer_attribute
{
	GLuint buffer;
	GLint size;
	GLenum type;
	GLboolean normalized;
	GLsizei stride;
	const void *pointer;
};

struct layer_vertex_array
{
	int used;
	unsigned int enabled;
	/* the attributes that are packed normals (normals_apply) */
	unsigned int packed;
	GLuint element_buffer;
	struct layer_attribute attributes[LAYER_ATTRIBUTES];
};

/* [0] is the default one */
static struct layer_vertex_array vertex_arrays[256];
static GLuint vertex_array_current;
static GLuint array_buffer;
static GLint attribute_maximum;

static struct layer_vertex_array *vertex_array(void)
{
	return &vertex_arrays[vertex_array_current];
}

/* what a packed normal attribute was last pointed at (normals_apply): the
GPU buffer and offset, or the client memory */
static struct
{
	GLuint gpu;
	const void *pointer;
} normals_applied[LAYER_ATTRIBUTES];

static int normals_flat(void);
static void attribute_point(GLuint index, const struct layer_attribute *attribute);

/* the attribute as a client array, in its buffer's memory */
static void attribute_apply(GLuint index, const struct layer_attribute *attribute)
{
	if (index < LAYER_ATTRIBUTES)
	{
		normals_applied[index].gpu = 0;
		normals_applied[index].pointer = NULL;
	}
	/* (a packed normal's copy is pointed at as the draw needs it) */
	if (index < LAYER_ATTRIBUTES && (vertex_array()->packed & (1u << index)) && !normals_flat())
		return;
	attribute_point(index, attribute);
}

static void attribute_point(GLuint index, const struct layer_attribute *attribute)
{
	struct layer_buffer *buffer = buffer_get(attribute->buffer);

	counts.pointers++;
	merge_changed_pointers = 1;
	if (buffer && buffer->gpu)
	{
		real.BindBuffer(GL_ARRAY_BUFFER, buffer->gpu);
		real.VertexAttribPointer(index, attribute->size ? attribute->size : 4,
			attribute->type ? attribute->type : GL_FLOAT, attribute->normalized, attribute->stride, attribute->pointer);
		return;
	}
	real.BindBuffer(GL_ARRAY_BUFFER, 0);
	real.VertexAttribPointer(index, attribute->size ? attribute->size : 4,
		attribute->type ? attribute->type : GL_FLOAT, attribute->normalized, attribute->stride,
		buffer_pointer(attribute->buffer, attribute->pointer));
}

static void GL_APIENTRY layer_GenVertexArrays(GLsizei count, GLuint *names)
{
	GLsizei index;
	GLuint name;

	for (index = 0; index < count; index++)
	{
		for (name = 1; name < sizeof(vertex_arrays) / sizeof(*vertex_arrays) && vertex_arrays[name].used; name++)
		{
		}
		if (name >= sizeof(vertex_arrays) / sizeof(*vertex_arrays))
		{
			LAYER_ONCE("out of vertex arrays");
			names[index] = 0;
			continue;
		}
		memset(&vertex_arrays[name], 0, sizeof(vertex_arrays[name]));
		vertex_arrays[name].used = 1;
		names[index] = name;
	}
}

static void GL_APIENTRY layer_BindVertexArray(GLuint name)
{
	struct layer_vertex_array *from = vertex_array(), *to;
	GLuint index;

	if (name >= sizeof(vertex_arrays) / sizeof(*vertex_arrays))
		name = 0;
	if (name == vertex_array_current)
		return;
	to = &vertex_arrays[name];
	vertex_array_current = name;
	for (index = 0; index < (GLuint)attribute_maximum && index < LAYER_ATTRIBUTES; index++)
	{
		unsigned int bit = 1u << index;

		if (to->enabled & bit)
		{
			attribute_apply(index, &to->attributes[index]);
			if (!(from->enabled & bit))
				real.EnableVertexAttribArray(index);
		}
		else if (from->enabled & bit)
		{
			real.DisableVertexAttribArray(index);
		}
	}
}

/* the binding a target names (the element array buffer is the vertex
array's) */
static GLuint *buffer_binding(GLenum target)
{
	switch (target)
	{
	case GL_ARRAY_BUFFER: return &array_buffer;
	case GL_ELEMENT_ARRAY_BUFFER: return &vertex_array()->element_buffer;
	case GL_COPY_READ_BUFFER: return &copy_read_buffer;
	case GL_COPY_WRITE_BUFFER: return &copy_write_buffer;
	default:
		/* (uniform, atomic counter buffers: none in ES 2) */
		LAYER_ONCE("no buffer target 0x%x", (unsigned)target);
		return NULL;
	}
}

static void GL_APIENTRY layer_BindBuffer(GLenum target, GLuint buffer)
{
	GLuint *binding = buffer_binding(target);

	if (binding)
		*binding = buffer;
}

static void GL_APIENTRY layer_EnableVertexAttribArray(GLuint index)
{
	if (index >= (GLuint)attribute_maximum)
	{
		LAYER_ONCE("vertex attribute %u past the box's %d", index, attribute_maximum);
		return;
	}
	vertex_array()->enabled |= 1u << index;
	real.EnableVertexAttribArray(index);
}

static void GL_APIENTRY layer_DisableVertexAttribArray(GLuint index)
{
	if (index >= (GLuint)attribute_maximum)
		return;
	vertex_array()->enabled &= ~(1u << index);
	real.DisableVertexAttribArray(index);
}

static void GL_APIENTRY layer_VertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
	GLsizei stride, const void *pointer)
{
	struct layer_attribute *attribute;

	if (index >= (GLuint)attribute_maximum || index >= LAYER_ATTRIBUTES)
		return;
	attribute = &vertex_array()->attributes[index];
	attribute->buffer = array_buffer;
	attribute->size = size;
	attribute->type = type;
	attribute->normalized = normalized;
	attribute->stride = stride;
	attribute->pointer = pointer;
	vertex_array()->packed &= ~(1u << index);
	attribute_apply(index, attribute);
}

static void GL_APIENTRY layer_VertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride,
	const void *pointer)
{
	struct layer_attribute *attribute;

	/* (the packed normals, D3DVSDT_NORMPACKED3, the renderer's only integer
	attribute: no integer attributes in ES 2, nor room in the skinned
	shaders to unpack them. Each draw reads a copy of them as normalized
	shorts, made once for the vertices it is of: normals_apply) */
	if (index >= (GLuint)attribute_maximum || index >= LAYER_ATTRIBUTES)
		return;
	if (size != 1 || (type != GL_UNSIGNED_INT && type != GL_INT))
	{
		LAYER_ONCE("integer vertex attributes read as floats");
		layer_VertexAttribPointer(index, size, type == GL_UNSIGNED_INT || type == GL_INT ? GL_UNSIGNED_SHORT : type,
			GL_FALSE, stride, pointer);
		return;
	}
	attribute = &vertex_array()->attributes[index];
	attribute->buffer = array_buffer;
	/* (as before, for HALO_GLES2_FLAT_NORMALS: read and not used) */
	attribute->size = 1;
	attribute->type = GL_UNSIGNED_SHORT;
	attribute->normalized = GL_FALSE;
	attribute->stride = stride ? stride : 4;
	attribute->pointer = pointer;
	vertex_array()->packed |= 1u << index;
	attribute_apply(index, attribute);
}

static void GL_APIENTRY layer_VertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w)
{
	if (index < (GLuint)attribute_maximum)
		real.VertexAttrib4f(index, (GLfloat)x, (GLfloat)y, (GLfloat)z, (GLfloat)w);
}

static int bones_window_needed(void);
static void bones_window_apply(unsigned long lowest, unsigned long highest);

/* the lowest and highest of count indices of type */
static void index_range(GLenum type, const void *indices, GLsizei count, unsigned long *lowest,
	unsigned long *highest)
{
	GLsizei index;

	*lowest = (unsigned long)-1;
	*highest = 0;
	for (index = 0; index < count; index++)
	{
		unsigned long value = type == GL_UNSIGNED_BYTE ? ((const GLubyte *)indices)[index] :
			type == GL_UNSIGNED_SHORT ? ((const GLushort *)indices)[index] : ((const GLuint *)indices)[index];

		if (value < *lowest)
			*lowest = value;
		if (value > *highest)
			*highest = value;
	}
}

static GLuint program_current;
/* the program the renderer made current (program_current is its flat
twin's while that stands in for it: twin_draw_begin), and the twins */
static GLuint program_renderer;
struct twin;
static struct twin *twins[LAYER_TRACKED];
static void twin_frame(void);
/* (the renderer's program made the driver's again after the layer's own; a
lazy program not yet loaded is not the driver's to use, program_ready) */
static int program_is_pending(GLuint program);
static void program_ready(GLuint program);
static void program_current_restore(void)
{
	if (!program_is_pending(program_current))
		real.UseProgram(program_current);
}
static GLuint read_framebuffer, draw_framebuffer;
/* each framebuffer's color texture (feedback_begin) */
static GLuint framebuffer_colors[LAYER_TRACKED];
static __typeof__(halo_glDrawArrays) real_DrawArrays;
static __typeof__(halo_glDrawElements) real_DrawElements;
static GLuint draw_framebuffer_seen(void);

/* A draw that samples the texture it draws into: the Xbox let a shader
read the render target (the menus' and the screen effects' passes), as do
desktop GL and Mali, but Vivante's GPU hangs on it until the kernel driver
resets it 20 seconds later. Those units sample a copy of the framebuffer
instead, made just before the draw. */
static GLuint feedback_texture;
static GLsizei feedback_width, feedback_height;
static void (GL_APIENTRY *real_CopyTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x,
	GLint y, GLsizei width, GLsizei height);

/* returns the units switched to the copy (a bit each), to put back after */
static unsigned int feedback_begin(void)
{
	GLuint color = draw_framebuffer < LAYER_TRACKED ? framebuffer_colors[draw_framebuffer] : 0;
	struct layer_texture *texture;
	unsigned int units = 0, unit;

	if (!draw_framebuffer || !color)
		return 0;
	for (unit = 0; unit < LAYER_TEXTURE_UNITS; unit++)
	{
		if (unit_textures[unit][0] == color)
			units |= 1u << unit;
	}
	if (!units)
		return 0;
	texture = texture_get(color);
	if (!texture || !texture->width || !texture->height || !real_CopyTexSubImage2D)
		return 0;
	LAYER_ONCE("a draw samples its own render target (texture %u): it samples a copy", color);
	real.ActiveTexture(GL_TEXTURE0);
	if (!feedback_texture)
		real_GenTextures(1, &feedback_texture);
	real.BindTexture(GL_TEXTURE_2D, feedback_texture);
	if (feedback_width != texture->width || feedback_height != texture->height)
	{
		real.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture->width, texture->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
			NULL);
		real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		feedback_width = texture->width;
		feedback_height = texture->height;
	}
	real_CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, texture->width, texture->height);
	for (unit = 0; unit < LAYER_TEXTURE_UNITS; unit++)
	{
		if (units & (1u << unit))
		{
			real.ActiveTexture(GL_TEXTURE0 + unit);
			real.BindTexture(GL_TEXTURE_2D, feedback_texture);
		}
	}
	real.ActiveTexture(GL_TEXTURE0 + active_unit);
	return units;
}

static void feedback_end(unsigned int units)
{
	unsigned int unit;

	if (!units)
		return;
	for (unit = 0; unit < LAYER_TEXTURE_UNITS; unit++)
	{
		if (units & (1u << unit))
		{
			real.ActiveTexture(GL_TEXTURE0 + unit);
			real.BindTexture(GL_TEXTURE_2D, unit_textures[unit][0] ? unit_textures[unit][0] : texture_white);
		}
	}
	real.ActiveTexture(GL_TEXTURE0 + active_unit);
}

/* One texture sampled from two units (the menus' widgets: their bitmap on
stages 0 and 1), with a mipmapped texture on another: Vivante's GPU hangs on
the draw until the kernel driver resets it 20 seconds later, whether the two
samplers read two units or one. With a different texture on the later unit
it does not, so that unit samples a copy: the texture drawn once into an
RGBA texture of its size (one level), kept until the texture changes. */
struct shared_copy
{
	GLuint texture, framebuffer;
	GLsizei width, height;
	/* the texture's uploads when the copy was made (layer_texture.generation) */
	unsigned long generation;
};

static struct shared_copy shared_copies[LAYER_TRACKED];

static int blit_program_make(void);
static GLint blit_texture_uniform, blit_rectangle_uniform;
static GLuint blit_program;

/* the copy of a texture, made (or made again) now: the state it changes is
put back as the draw it is made for had it */
static GLuint shared_copy_get(GLuint name)
{
	static const GLfloat corners[] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
	static const GLfloat whole[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	static const GLenum capabilities[] = { GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE, GL_SCISSOR_TEST,
		GL_DITHER };
	struct layer_texture *texture = texture_get(name);
	struct shared_copy *copy;
	struct layer_vertex_array *current = vertex_array();
	GLboolean enabled[sizeof(capabilities) / sizeof(*capabilities)], mask[4];
	GLint viewport[4];
	GLuint index;

	if (!texture || !texture->width || !texture->height || name >= LAYER_TRACKED || !blit_program_make())
		return 0;
	copy = &shared_copies[name];
	if (copy->texture && copy->width == texture->width && copy->height == texture->height &&
		copy->generation == texture->generation)
	{
		return copy->texture;
	}

	if (!copy->texture)
	{
		real_GenTextures(1, &copy->texture);
		halo_glGenFramebuffers(1, &copy->framebuffer);
	}
	real.ActiveTexture(GL_TEXTURE0);
	real.BindTexture(GL_TEXTURE_2D, copy->texture);
	real.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture->width, texture->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	real.BindFramebuffer(GL_FRAMEBUFFER, copy->framebuffer);
	real.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, copy->texture, 0);

	/* the state the draw had */
	for (index = 0; index < sizeof(capabilities) / sizeof(*capabilities); index++)
	{
		enabled[index] = halo_glIsEnabled(capabilities[index]);
		halo_glDisable(capabilities[index]);
	}
	real.GetIntegerv(GL_VIEWPORT, viewport);
	{
		GLint values[4];

		real.GetIntegerv(GL_COLOR_WRITEMASK, values);
		for (index = 0; index < 4; index++)
			mask[index] = (GLboolean)values[index];
	}
	halo_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	halo_glViewport(0, 0, texture->width, texture->height);
	for (index = 1; index < (GLuint)attribute_maximum; index++)
	{
		if (current->enabled & (1u << index))
			real.DisableVertexAttribArray(index);
	}

	/* the copy, drawn from the texture as it is (its own filter: level 0
	of a mipmapped one) */
	real.UseProgram(blit_program);
	real.BindTexture(GL_TEXTURE_2D, name);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	texture->applied_min = texture->applied_mag = 0;
	halo_glUniform1i(blit_texture_uniform, 0);
	halo_glUniform4fv(blit_rectangle_uniform, 1, whole);
	real.BindBuffer(GL_ARRAY_BUFFER, 0);
	real.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
	if (!(current->enabled & 1))
		real.EnableVertexAttribArray(0);
	real_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	/* back as the draw had it */
	if (current->enabled & 1)
		attribute_apply(0, &current->attributes[0]);
	else
		real.DisableVertexAttribArray(0);
	for (index = 1; index < (GLuint)attribute_maximum; index++)
	{
		if (current->enabled & (1u << index))
			real.EnableVertexAttribArray(index);
	}
	program_current_restore();
	real.BindFramebuffer(GL_FRAMEBUFFER, draw_framebuffer);
	halo_glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	halo_glColorMask(mask[0], mask[1], mask[2], mask[3]);
	for (index = 0; index < sizeof(capabilities) / sizeof(*capabilities); index++)
	{
		if (enabled[index])
			halo_glEnable(capabilities[index]);
	}
	real.BindTexture(GL_TEXTURE_2D, unit_textures[0][0] ? unit_textures[0][0] : texture_white);
	/* (the texture's filter, as its sampler has it, at its next binding) */
	sampler_apply(0);
	real.ActiveTexture(GL_TEXTURE0 + active_unit);

	copy->width = texture->width;
	copy->height = texture->height;
	copy->generation = texture->generation;
	LAYER_ONCE("a texture sampled from two units: the later samples a copy");
	return copy->texture;
}

/* the later units that share a texture with an earlier one, given its copy
for the draw; returns them, to put back (shared_units_end) */
static unsigned int shared_units_begin(void)
{
	unsigned int moved = 0, unit, earlier;

	for (unit = 1; unit < 4; unit++)
	{
		GLuint copy;

		if (!unit_textures[unit][0])
			continue;
		for (earlier = 0; earlier < unit; earlier++)
		{
			if (unit_textures[earlier][0] == unit_textures[unit][0])
				break;
		}
		if (earlier == unit)
			continue;
		copy = shared_copy_get(unit_textures[unit][0]);
		if (!copy)
			continue;
		real.ActiveTexture(GL_TEXTURE0 + unit);
		real.BindTexture(GL_TEXTURE_2D, copy);
		real.ActiveTexture(GL_TEXTURE0 + active_unit);
		moved |= 1u << unit;
	}
	return moved;
}

static void shared_units_end(unsigned int moved)
{
	unsigned int unit;

	for (unit = 1; unit < 4; unit++)
	{
		if (moved & (1u << unit))
		{
			real.ActiveTexture(GL_TEXTURE0 + unit);
			real.BindTexture(GL_TEXTURE_2D, unit_textures[unit][0] ? unit_textures[unit][0] : texture_white);
			real.ActiveTexture(GL_TEXTURE0 + active_unit);
		}
	}
}

/* A small draw whose vertices all have the same position (attribute 0,
the Xbox's v0): no pixel of it is drawn, but with textures bound
Vivante's GPU can hang on it until the kernel driver resets it 20 seconds
later (the menus draw one every frame). Left out. */
static int draw_degenerate(GLint first_vertex, GLsizei count, GLenum type, const void *indices)
{
	const struct layer_vertex_array *current = vertex_array();
	const struct layer_attribute *position = &current->attributes[0];
	const unsigned char *base;
	size_t size, stride;
	GLsizei number;

	if (!(current->enabled & 1) || count <= 0 || count > 64 || position->type != GL_FLOAT)
		return 0;
	base = buffer_pointer(position->buffer, position->pointer);
	if (!base)
		return 0;
	size = (size_t)(position->size ? position->size : 4) * sizeof(float);
	stride = position->stride ? (size_t)position->stride : size;
	for (number = 1; number < count; number++)
	{
		size_t first = (size_t)first_vertex, vertex = (size_t)first_vertex + (size_t)number;

		if (indices)
		{
			first = type == GL_UNSIGNED_INT ? ((const GLuint *)indices)[0] : type == GL_UNSIGNED_SHORT ?
				((const GLushort *)indices)[0] : ((const GLubyte *)indices)[0];
			vertex = type == GL_UNSIGNED_INT ? ((const GLuint *)indices)[number] : type == GL_UNSIGNED_SHORT ?
				((const GLushort *)indices)[number] : ((const GLubyte *)indices)[number];
		}
		if (memcmp(base + first * stride, base + vertex * stride, size))
			return 0;
	}
	LAYER_ONCE("draws whose vertices are all in one place are left out (the GPU can hang on them)");
	return 1;
}

static int program_skipped(GLuint program);
static void skin_log(GLsizei count, GLenum type, const void *indices);

/* HALO_GLES2_EXPERIMENT=program:bits, for one program's draws while
hunting a GPU hang: 1, its textures a white 1x1; 2, only the attributes the
program reads enabled; 4, not drawn at all; 64, drawn into one pixel (the
scissor), for what filling costs; 128, only its first triangle, for what a
draw costs whatever its size. Program 0 is every program.
experiment_begin returns what to put back (experiment_end). */
static GLuint experiment_program, experiment_white;
static unsigned int experiment_bits;
static GLint experiment_scissor[4];
static GLboolean experiment_scissoring;

static unsigned int experiment_begin(void)
{
	static int read;
	unsigned int undo = 0, unit;

	if (!read)
	{
		const char *value = getenv("HALO_GLES2_EXPERIMENT");

		read = 1;
		if (value && strchr(value, ':'))
		{
			experiment_program = (GLuint)atoi(value);
			experiment_bits = (unsigned int)atoi(strchr(value, ':') + 1);
			platform_log("gles2: experiment on program %u: %u", experiment_program, experiment_bits);
		}
	}
	if (!experiment_bits || (experiment_program && program_current != experiment_program))
		return 0;
	if (experiment_bits & (1 | 8 | 16 | 32))
	{
		static const unsigned char white[4] = { 255, 255, 255, 255 };

		if (!experiment_white)
		{
			real_GenTextures(1, &experiment_white);
			real.ActiveTexture(GL_TEXTURE0);
			real.BindTexture(GL_TEXTURE_2D, experiment_white);
			real.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
			real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		}
		/* (1: units 0 and 1, 8: units 2 and 3) */
		for (unit = 0; unit < 4; unit++)
		{
			/* (16: unit 1 alone, 32: unit 0 alone) */
			if (!(experiment_bits & (unit < 2 ? 1 : 8)) && !(unit == 1 && (experiment_bits & 16)) &&
				!(unit == 0 && (experiment_bits & 32)))
			{
				continue;
			}
			real.ActiveTexture(GL_TEXTURE0 + unit);
			real.BindTexture(GL_TEXTURE_2D, experiment_white);
		}
		real.ActiveTexture(GL_TEXTURE0 + active_unit);
		undo |= 1;
	}
	if (experiment_bits & 2)
	{
		GLint count = 0, index;
		unsigned int used = 0;
		struct layer_vertex_array *current = vertex_array();

		static void (GL_APIENTRY *get_active)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *);
		static GLint (GL_APIENTRY *get_location)(GLuint, const GLchar *);

		if (!get_active)
		{
			get_active = (__typeof__(get_active))SDL_GL_GetProcAddress("glGetActiveAttrib");
			get_location = (__typeof__(get_location))SDL_GL_GetProcAddress("glGetAttribLocation");
		}
		halo_glGetProgramiv(program_current, GL_ACTIVE_ATTRIBUTES, &count);
		for (index = 0; index < count; index++)
		{
			char name[64];
			GLint size;
			GLenum type;
			GLint location;

			get_active(program_current, (GLuint)index, sizeof(name), NULL, &size, &type, name);
			location = get_location(program_current, name);
			if (location >= 0)
				used |= 1u << location;
		}
		for (unit = 0; unit < (unsigned int)attribute_maximum; unit++)
		{
			if ((current->enabled & (1u << unit)) && !(used & (1u << unit)))
				real.DisableVertexAttribArray(unit);
		}
		undo |= 2;
	}
	if (experiment_bits & 4)
		undo |= 4;
	if (experiment_bits & 64)
	{
		static GLboolean (GL_APIENTRY *is_enabled)(GLenum);
		static void (GL_APIENTRY *enable)(GLenum);
		static void (GL_APIENTRY *scissor)(GLint, GLint, GLsizei, GLsizei);

		if (!scissor)
		{
			is_enabled = (__typeof__(is_enabled))SDL_GL_GetProcAddress("glIsEnabled");
			enable = (__typeof__(enable))SDL_GL_GetProcAddress("glEnable");
			scissor = (__typeof__(scissor))SDL_GL_GetProcAddress("glScissor");
		}
		experiment_scissoring = is_enabled(GL_SCISSOR_TEST);
		halo_glGetIntegerv(GL_SCISSOR_BOX, experiment_scissor);
		enable(GL_SCISSOR_TEST);
		scissor(0, 0, 1, 1);
		undo |= 64;
	}
	return undo;
}

static void experiment_end(unsigned int undo)
{
	unsigned int unit;

	if (undo & 1)
	{
		for (unit = 0; unit < 4; unit++)
		{
			real.ActiveTexture(GL_TEXTURE0 + unit);
			real.BindTexture(GL_TEXTURE_2D, unit_textures[unit][0] ? unit_textures[unit][0] : texture_white);
		}
		real.ActiveTexture(GL_TEXTURE0 + active_unit);
	}
	if (undo & 2)
	{
		struct layer_vertex_array *current = vertex_array();

		for (unit = 0; unit < (unsigned int)attribute_maximum; unit++)
		{
			if (current->enabled & (1u << unit))
				real.EnableVertexAttribArray(unit);
		}
	}
	if (undo & 64)
	{
		static void (GL_APIENTRY *disable)(GLenum);
		static void (GL_APIENTRY *scissor)(GLint, GLint, GLsizei, GLsizei);

		if (!scissor)
		{
			disable = (__typeof__(disable))SDL_GL_GetProcAddress("glDisable");
			scissor = (__typeof__(scissor))SDL_GL_GetProcAddress("glScissor");
		}
		scissor(experiment_scissor[0], experiment_scissor[1], experiment_scissor[2], experiment_scissor[3]);
		if (!experiment_scissoring)
			disable(GL_SCISSOR_TEST);
	}
}

/* HALO_GLES2_SLOW: every draw finished (glFinish) and timed, and one that
took the GPU past a second in the log with what it drew with: Vivante's
kernel driver resets a hung GPU after 20 seconds, so a draw that hangs it
shows as a frame of 20 */
static int slow_checking = -1;

static void program_dump(GLuint program, const char *tag);
static void perf_ab(int frame, Uint64 took);

static void draw_slow_check(const char *kind, GLenum mode, GLsizei count, GLenum type, Uint64 start)
{
	static int reported;
	Uint64 took;
	struct layer_vertex_array *current;
	char attributes[160];
	size_t length = 0;
	GLuint index;

	static long threshold = -1;

	/* (HALO_GLES2_SLOW=ms: the draws past that many, a line each, for where
	the GPU's time goes; 1 or no number, the hangs only) */
	if (threshold < 0)
		threshold = atol(getenv("HALO_GLES2_SLOW")) > 1 ? atol(getenv("HALO_GLES2_SLOW")) : 1000;
	halo_glFinish();
	took = (SDL_GetTicksNS() - start) / 1000;
	/* (and with a number, every draw's time by program, the most costly
	each 10 seconds) */
	if (threshold < 1000)
	{
		static unsigned long program_ms[LAYER_TRACKED], program_draws[LAYER_TRACKED];
		static Uint64 window_start;
		Uint64 now = SDL_GetTicks();

		if (program_current < LAYER_TRACKED)
		{
			program_ms[program_current] += (unsigned long)took;
			program_draws[program_current]++;
		}
		if (!window_start)
			window_start = now;
		if (now - window_start >= 10000)
		{
			unsigned long total = 0;
			int rank;

			for (index = 0; index < LAYER_TRACKED; index++)
				total += program_ms[index];
			platform_log("gles2 gpu: %lu ms of draws in %llu ms", total / 1000, (unsigned long long)(now - window_start));
			for (rank = 0; rank < 8; rank++)
			{
				GLuint best = 0;

				for (index = 1; index < LAYER_TRACKED; index++)
				{
					if (program_ms[index] > program_ms[best])
						best = index;
				}
				if (!program_ms[best])
					break;
				platform_log("gles2 gpu:   program %u: %lu ms in %lu draws", best, program_ms[best] / 1000,
					program_draws[best]);
				if (getenv("HALO_GLES2_DUMP"))
					program_dump(best, "gpu");
				program_ms[best] = 0;
			}
			memset(program_ms, 0, sizeof(program_ms));
			memset(program_draws, 0, sizeof(program_draws));
			window_start = now;
		}
	}
	/* (took in microseconds, for the draws under a millisecond) */
	took /= 1000;
	if ((long)took < threshold || reported >= 400)
		return;
	reported++;
	if (threshold < 1000)
	{
		struct layer_texture *texture = texture_get(unit_textures[0][0]);

		platform_log("gles2 slow: %llu ms, %s mode %u count %d, program %u, framebuffer %u, texture 0 %ux%u 0x%x",
			(unsigned long long)took, kind, (unsigned)mode, (int)count, program_current, draw_framebuffer_seen(),
			texture ? (unsigned)texture->width : 0, texture ? (unsigned)texture->height : 0,
			texture ? (unsigned)texture->format : 0);
		if (getenv("HALO_GLES2_DUMP"))
			program_dump(program_current, "slow");
		return;
	}
	current = vertex_array();
	attributes[0] = 0;
	for (index = 0; index < (GLuint)attribute_maximum && length + 24 < sizeof(attributes); index++)
	{
		const struct layer_attribute *attribute = &current->attributes[index];

		if (current->enabled & (1u << index))
			length += (size_t)snprintf(attributes + length, sizeof(attributes) - length, " %u:%dx%04x/%d", index,
				(int)attribute->size, (unsigned)attribute->type, (int)attribute->stride);
	}
	platform_log("gles2: a %s took %llu ms: mode %u count %d type 0x%x program %u framebuffer %u, "
		"textures %u %u %u %u, attributes%s", kind, (unsigned long long)took, (unsigned)mode, (int)count,
		(unsigned)type, program_current, draw_framebuffer_seen(), unit_textures[0][0], unit_textures[1][0],
		unit_textures[2][0], unit_textures[3][0], attributes);
	for (index = 0; index < 4; index++)
	{
		struct layer_texture *texture = texture_get(unit_textures[index][0]);

		if (texture)
			platform_log("gles2:   unit %u: texture %u, %dx%d, format 0x%x, levels to %d, min filter 0x%x (given 0x%x), "
				"swizzle %x %x %x %x", index, unit_textures[index][0], (int)texture->width, (int)texture->height,
				(unsigned)texture->format, (int)texture->max_level, (unsigned)texture->applied_min,
				(unsigned)texture->min_filter, (unsigned)texture->swizzle[0], (unsigned)texture->swizzle[1],
				(unsigned)texture->swizzle[2], (unsigned)texture->swizzle[3]);
	}
	program_dump(program_current, "slow");
	/* its vertices, as the attributes the program reads give them */
	for (index = 0; index < (GLuint)attribute_maximum; index++)
	{
		const struct layer_attribute *attribute = &current->attributes[index];
		const float *vertex = buffer_pointer(attribute->buffer, attribute->pointer);
		int number;

		if (!(current->enabled & (1u << index)) || !vertex || attribute->type != GL_FLOAT || mode != GL_TRIANGLE_FAN)
			continue;
		for (number = 0; number < count && number < 4; number++)
		{
			const float *value = (const float *)((const unsigned char *)vertex + number * attribute->stride);

			platform_log("gles2:   attribute %u vertex %d: %g %g %g %g", index, number, value[0], value[1], value[2],
				value[3]);
		}
	}
	{
		static void (GL_APIENTRY *get_uniform)(GLuint, GLint, GLfloat *);
		static const char *const names[] = { "texture_scale[0]", "texture_scale[1]", "texture_scale[2]",
			"texture_lod_bias", "viewport_scale", "viewport_offset" };
		GLuint name;

		if (!get_uniform)
			get_uniform = (__typeof__(get_uniform))SDL_GL_GetProcAddress("glGetUniformfv");
		for (name = 0; name < sizeof(names) / sizeof(*names); name++)
		{
			GLint location = halo_glGetUniformLocation(program_current, names[name]);
			GLfloat value[4] = { 0 };

			if (location < 0 || !get_uniform)
				continue;
			get_uniform(program_current, location, value);
			platform_log("gles2:   %s: %g %g %g %g", names[name], value[0], value[1], value[2], value[3]);
		}
	}
}

static void draw_traced(const char *kind, GLenum mode, GLsizei count)
{
	GLenum error;

	if (!tracing())
		return;
	halo_glFinish();
	error = halo_glGetError();
	trace("%s mode %u count %d program %u framebuffer %u: done, error 0x%x", kind, (unsigned)mode, (int)count,
		program_current, draw_framebuffer_seen(), (unsigned)error);
}

/* a glFlush after every FLUSH_EVERY draws (HALO_GLES2_FLUSH_EVERY=n for
another n, 0 for none). The driver keeps a frame's commands until the swap,
the GPU then working through all of them while the CPU waits (12 of
present's 14 ms at Bloodgulch's spawn); a flush on the way sends what there
is, so the GPU starts while the CPU is still on the rest of the frame. There,
in performance mode, a frame went from 46.6 ms to 42.8 every 12 draws, 43.3
every 16, 44.3 every 8 (each flush waits a little) and 44.5 every 24 */
#define FLUSH_EVERY 12
static void draw_flush_maybe(void)
{
	static long every = -1;
	static void (GL_APIENTRY *flush)(void);

	if (every < 0)
	{
		const char *value = getenv("HALO_GLES2_FLUSH_EVERY");

		every = value ? atol(value) : FLUSH_EVERY;
		flush = (__typeof__(flush))SDL_GL_GetProcAddress("glFlush");
		platform_log("gles2: a flush every %ld draws", every);
	}
	if (every > 0 && flush && draws_total % (unsigned long)every == 0)
		flush();
}

/* ---------- the packed normals

The models' normals (and binormals, tangents) are D3DVSDT_NORMPACKED3, 11,
11 and 10 bits in a 32-bit word, which ES 2 has no integer attributes for
and the box's compiler no room to unpack in the skinned shaders. A draw
reads a copy of them as normalized shorts instead (the shader's
unpack_normpacked3 then only passes them on). A stream in the mirror's
buffers (the models' own vertices, drawn every frame) has its copy in a GPU
buffer, made at its first draw and again only when the game has written
its pages since (buffer_pages_uploaded) or a draw reaches past it; the
indices' range of a draw from the mirror is kept the same way. A streamed
draw's are unpacked for the draw. HALO_GLES2_FLAT_NORMALS: every normal
straight up, as before. */

static int normals_flat(void)
{
	static int flat = -1;

	if (flat < 0)
		flat = getenv("HALO_GLES2_FLAT_NORMALS") != NULL;
	return flat;
}

static void normals_unpack(const unsigned char *source, GLsizei stride, unsigned long first, unsigned long last,
	GLshort *to)
{
	unsigned long vertex;

	for (vertex = first; vertex <= last; vertex++, to += 4)
	{
		uint32_t packed;
		int x, y, z;

		memcpy(&packed, source + vertex * (unsigned long)stride, 4);
		x = (int32_t)(packed << 21) >> 21;
		y = (int32_t)(packed << 10) >> 21;
		z = (int32_t)packed >> 22;
		/* (x / 1023, y / 1023, z / 511, as nv2a_vsh.c's, in shorts) */
		x = (x * 32767 + (x < 0 ? -511 : 511)) / 1023;
		y = (y * 32767 + (y < 0 ? -511 : 511)) / 1023;
		z = (z * 32767 + (z < 0 ? -255 : 255)) / 511;
		to[0] = (GLshort)(x < -32767 ? -32767 : x > 32767 ? 32767 : x);
		to[1] = (GLshort)(y < -32767 ? -32767 : y > 32767 ? 32767 : y);
		to[2] = (GLshort)(z < -32767 ? -32767 : z > 32767 ? 32767 : z);
		to[3] = 32767;
	}
}

#define NORMAL_STREAMS 1024
#define NORMAL_RANGES 2048

static struct normal_stream
{
	GLuint buffer;
	uintptr_t pointer;
	GLsizei stride;
	GLuint gpu;
	/* vertices 0 to this less one copied, as of generation built (checked:
	the generation its pages were last found unchanged at) */
	unsigned long vertices;
	unsigned int built, checked;
	/* draws_total at its latest draw */
	unsigned long used;
} normal_streams[NORMAL_STREAMS];

static unsigned long normals_hash(unsigned long a, unsigned long b, unsigned long c)
{
	unsigned long hash = (unsigned long)((a * 2654435761u) ^ (b * 0x9e3779b1u) ^ (c * 0x85ebca6bu));

	hash ^= hash >> 15;
	hash *= 0x2c1b3c6du;
	return hash ^ (hash >> 13);
}

static struct normal_range
{
	GLuint buffer;
	uintptr_t offset;
	GLsizei count;
	GLenum type;
	unsigned long lowest, highest;
	unsigned int built, checked;
} normal_ranges[NORMAL_RANGES];

static struct
{
	unsigned long draws, copies, copied, streamed, flat, bytes, other, stale, further;
} normals_counts;

static GLshort *normals_scratch;
static unsigned long normals_scratch_vertices;

static GLshort *normals_scratch_get(unsigned long vertices)
{
	if (vertices > normals_scratch_vertices)
	{
		GLshort *grown = realloc(normals_scratch, vertices * 4 * sizeof(GLshort));

		if (!grown)
			return NULL;
		normals_scratch = grown;
		normals_scratch_vertices = vertices;
	}
	return normals_scratch;
}

/* every normal straight up, for 65536 vertices: a draw whose copy could not
be made */
static GLuint normals_flat_buffer(void)
{
	static GLuint gpu;

	if (!gpu)
	{
		GLshort *flat = malloc(65536 * 4 * sizeof(GLshort));
		unsigned long vertex;

		if (!flat)
			return 0;
		for (vertex = 0; vertex < 65536; vertex++)
		{
			flat[vertex * 4] = flat[vertex * 4 + 1] = 0;
			flat[vertex * 4 + 2] = flat[vertex * 4 + 3] = 32767;
		}
		real_GenBuffers(1, &gpu);
		real.BindBuffer(GL_ARRAY_BUFFER, gpu);
		real_BufferData(GL_ARRAY_BUFFER, 65536 * 4 * sizeof(GLshort), flat, GL_STATIC_DRAW);
		free(flat);
		normals_counts.bytes += 65536 * 4 * sizeof(GLshort);
	}
	return gpu;
}

/* an indexed draw's lowest and highest index, kept for indices in the
mirror */
static void normals_index_range(GLenum type, const void *indices, GLsizei count, GLuint element_buffer,
	const void *offset, unsigned long *lowest, unsigned long *highest)
{
	const struct layer_buffer *elements = buffer_get(element_buffer);
	struct normal_range *range;
	unsigned long bytes = (unsigned long)count * (type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4);

	if (!elements || !elements->pages)
	{
		index_range(type, indices, count, lowest, highest);
		return;
	}
	range = &normal_ranges[normals_hash((uintptr_t)offset, element_buffer, (unsigned long)count) % NORMAL_RANGES];
	if (range->buffer == element_buffer && range->offset == (uintptr_t)offset && range->count == count &&
		range->type == type && (range->checked == upload_generation ||
		buffer_pages_unchanged(elements, (uintptr_t)offset, (uintptr_t)offset + bytes, range->built)))
	{
		range->checked = upload_generation;
		*lowest = range->lowest;
		*highest = range->highest;
		return;
	}
	index_range(type, indices, count, lowest, highest);
	range->buffer = element_buffer;
	range->offset = (uintptr_t)offset;
	range->count = count;
	range->type = type;
	range->lowest = *lowest;
	range->highest = *highest;
	range->built = range->checked = upload_generation;
}

/* the copy of a mirror stream's normals, vertices 0 to highest: its GPU
buffer, or 0 */
static GLuint normals_stream(const struct layer_attribute *attribute, const struct layer_buffer *buffer,
	unsigned long highest)
{
	uintptr_t pointer = (uintptr_t)attribute->pointer;
	unsigned long slot = normals_hash(pointer, attribute->buffer, (unsigned long)attribute->stride) % NORMAL_STREAMS;
	struct normal_stream *stream = NULL;
	unsigned long vertices = highest + 1;
	int same, probe;
	GLshort *copy;

	/* (four ways: the stream's own, else an empty one, else the one drawn
	least lately) */
	for (probe = 0; probe < 4 && !stream; probe++)
	{
		struct normal_stream *way = &normal_streams[(slot + (unsigned long)probe) % NORMAL_STREAMS];

		if (way->gpu && way->buffer == attribute->buffer && way->pointer == pointer &&
			way->stride == attribute->stride)
		{
			stream = way;
		}
	}
	for (probe = 0; probe < 4 && !stream; probe++)
	{
		struct normal_stream *way = &normal_streams[(slot + (unsigned long)probe) % NORMAL_STREAMS];

		if (!way->gpu)
			stream = way;
	}
	if (!stream)
	{
		stream = &normal_streams[slot];
		for (probe = 1; probe < 4; probe++)
		{
			struct normal_stream *way = &normal_streams[(slot + (unsigned long)probe) % NORMAL_STREAMS];

			if (way->used < stream->used)
				stream = way;
		}
	}
	stream->used = draws_total;
	same = stream->gpu && stream->buffer == attribute->buffer && stream->pointer == pointer &&
		stream->stride == attribute->stride;

	if (same && vertices <= stream->vertices && (stream->checked == upload_generation ||
		buffer_pages_unchanged(buffer, pointer, pointer + (stream->vertices - 1) * (unsigned long)attribute->stride + 4,
		stream->built)))
	{
		stream->checked = upload_generation;
		return stream->gpu;
	}
	/* (the same stream again, further: as far as either) */
	if (same && stream->vertices > vertices)
		vertices = stream->vertices;
	if (!same)
		normals_counts.other++;
	else if (vertices > stream->vertices)
		normals_counts.further++;
	else
		normals_counts.stale++;
	if (pointer + (vertices - 1) * (unsigned long)attribute->stride + 4 > (unsigned long)buffer->size)
		return 0;
	copy = normals_scratch_get(vertices);
	if (!copy)
		return 0;
	normals_unpack(buffer->data + pointer, attribute->stride, 0, vertices - 1, copy);
	if (!stream->gpu)
		real_GenBuffers(1, &stream->gpu);
	else
		normals_counts.bytes -= stream->vertices * 4 * sizeof(GLshort);
	if (!stream->gpu)
		return 0;
	real.BindBuffer(GL_ARRAY_BUFFER, stream->gpu);
	real_BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertices * 4 * sizeof(GLshort)), copy, GL_STATIC_DRAW);
	normals_counts.bytes += vertices * 4 * sizeof(GLshort);
	normals_counts.copies++;
	normals_counts.copied += vertices;
	stream->buffer = attribute->buffer;
	stream->pointer = pointer;
	stream->stride = attribute->stride;
	stream->vertices = vertices;
	stream->built = stream->checked = upload_generation;
	return stream->gpu;
}

/* the attributes a program reads, a bit each (with bit 31 for known; 0
again when it is linked) */
static unsigned int program_attributes[LAYER_TRACKED];

static unsigned int program_reads(GLuint program)
{
	static void (GL_APIENTRY *get_active)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *);
	static GLint (GL_APIENTRY *get_location)(GLuint, const GLchar *);
	GLint count = 0, index;
	unsigned int used = 1u << 31;

	if (!program || program >= LAYER_TRACKED)
		return ~0u;
	if (program_attributes[program])
		return program_attributes[program];
	if (!get_active)
	{
		get_active = (__typeof__(get_active))SDL_GL_GetProcAddress("glGetActiveAttrib");
		get_location = (__typeof__(get_location))SDL_GL_GetProcAddress("glGetAttribLocation");
	}
	if (!get_active || !get_location)
		return ~0u;
	halo_glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &count);
	for (index = 0; index < count; index++)
	{
		char name[64];
		GLint size;
		GLenum type;
		GLint location;

		get_active(program, (GLuint)index, sizeof(name), NULL, &size, &type, name);
		location = get_location(program, name);
		if (location >= 0 && location < 31)
			used |= 1u << location;
	}
	program_attributes[program] = used;
	return used;
}

/* points each packed normal attribute of the draw (vertices lowest to
highest) that its program reads at its copy; the others at the renderer's
own vertices, as before (the GPU fetches every enabled attribute, and the
copies are buffers of their own) */
static void normals_apply(unsigned long lowest, unsigned long highest)
{
	struct layer_vertex_array *array = vertex_array();
	unsigned int packed = array->packed & array->enabled;
	unsigned int reads;
	GLuint index;

	if (normals_flat())
		return;
	reads = program_reads(program_current);
	for (index = 0; packed && index < LAYER_ATTRIBUTES; index++)
	{
		const struct layer_attribute *attribute = &array->attributes[index];
		const struct layer_buffer *buffer;
		GLuint gpu = 0;
		const void *pointer = NULL;

		if (!(packed & (1u << index)))
			continue;
		packed &= ~(1u << index);
		if (!(reads & (1u << index)))
		{
			if (normals_applied[index].gpu != ~0u)
			{
				attribute_point(index, attribute);
				normals_applied[index].gpu = ~0u;
				normals_applied[index].pointer = NULL;
			}
			continue;
		}
		normals_counts.draws++;
		buffer = buffer_get(attribute->buffer);
		if (highest < lowest)
		{
		}
		else if (buffer && buffer->gpu && buffer->pages && buffer->data)
		{
			gpu = normals_stream(attribute, buffer, highest);
		}
		else
		{
			const unsigned char *source = buffer_pointer(attribute->buffer, attribute->pointer);
			GLshort *copy = source && highest - lowest < 65536 ? normals_scratch_get(highest - lowest + 1) : NULL;

			if (copy)
			{
				normals_unpack(source, attribute->stride, lowest, highest, copy);
				pointer = copy - lowest * 4;
				normals_counts.streamed += highest - lowest + 1;
			}
		}
		if (!gpu && !pointer)
		{
			gpu = highest < 65536 ? normals_flat_buffer() : 0;
			normals_counts.flat++;
			if (!gpu)
				continue;
		}
		/* (a client copy is the scratch, rewritten each draw: given again
		each time) */
		if (gpu && normals_applied[index].gpu == gpu)
			continue;
		normals_applied[index].gpu = gpu;
		normals_applied[index].pointer = pointer;
		counts.pointers++;
		merge_changed_pointers = 1;
		real.BindBuffer(GL_ARRAY_BUFFER, gpu);
		real.VertexAttribPointer(index, 4, GL_SHORT, GL_TRUE, 4 * sizeof(GLshort), gpu ? NULL : pointer);
	}
}

/* every 300 frames, with HALO_GLES2_NORMALS_LOG */
static void normals_frame(void)
{
	static unsigned long frames;
	static int logging = -1;

	if (logging < 0)
		logging = getenv("HALO_GLES2_NORMALS_LOG") != NULL;
	if (!logging || ++frames % 300)
		return;
	platform_log("gles2 normals: %lu draws a frame, %lu copies made (%lu vertices), %lu streamed vertices a frame, "
		"%lu flat draws a frame, %lu KB of copies; copies for another stream %lu, stale %lu, further %lu",
		normals_counts.draws / 300, normals_counts.copies,
		normals_counts.copied, normals_counts.streamed / 300, normals_counts.flat / 300, normals_counts.bytes / 1024,
		normals_counts.other, normals_counts.stale, normals_counts.further);
	normals_counts.other = normals_counts.stale = normals_counts.further = 0;
	normals_counts.draws = normals_counts.copies = normals_counts.copied = normals_counts.streamed = 0;
	normals_counts.flat = 0;
}

static void twin_draw_begin(void);

static void GL_APIENTRY layer_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
	twin_draw_begin();
	program_ready(program_current);
	merge_stats_draw(program_current);
	counts.draws++;
	draws_total++;
	Uint64 start = 0;

	if (slow_checking < 0)
		slow_checking = getenv("HALO_GLES2_SLOW") != NULL;
	if (slow_checking)
	{
		halo_glFinish();
		start = SDL_GetTicksNS();
	}
	if (profiling)
		profile_report();
	if (tracing())
		trace("draw arrays mode %u count %d program %u", (unsigned)mode, (int)count, program_current);
	border_uniforms_apply();
	position_uniforms_apply();
	if (count > 0 && first >= 0 && bones_window_needed())
		bones_window_apply((unsigned long)first, (unsigned long)first + (unsigned long)count - 1);
	if (count > 0 && first >= 0 && (vertex_array()->packed & vertex_array()->enabled))
		normals_apply((unsigned long)first, (unsigned long)first + (unsigned long)count - 1);
	{
		unsigned int feedback = feedback_begin();
		unsigned int experiment = experiment_begin();

		if (!(experiment & 4) && !draw_degenerate(first, count, 0, NULL) && !program_skipped(program_current))
		{
			unsigned int shared = shared_units_begin();

			real_DrawArrays(mode, first, (experiment_bits & 128) && count > 3 ? 3 : count);
			shared_units_end(shared);
		}
		experiment_end(experiment);
		feedback_end(feedback);
	}
	if (slow_checking)
		draw_slow_check("draw arrays", mode, count, 0, start);
	draw_traced("draw arrays", mode, count);
	draw_flush_maybe();
}

/* (for the log: whether an indexed draw's vertices are client arrays, which
the driver copies to the GPU each draw, and how many bytes of them) */
static void client_vertices_count(GLenum type, const void *indices, GLsizei count)
{
	const struct layer_vertex_array *array = vertex_array();
	unsigned long lowest = 0, highest = 0, bytes = 0;
	int ranged = 0;
	GLuint index;

	for (index = 0; index < (GLuint)attribute_maximum && index < LAYER_ATTRIBUTES; index++)
	{
		const struct layer_attribute *attribute = &array->attributes[index];
		const struct layer_buffer *buffer;

		if (!(array->enabled & (1u << index)))
			continue;
		buffer = buffer_get(attribute->buffer);
		if (buffer && buffer->gpu)
			continue;
		if (!ranged)
		{
			index_range(type, indices, count, &lowest, &highest);
			ranged = 1;
		}
		bytes += (highest - lowest + 1) * (unsigned long)(attribute->stride ? attribute->stride : 16);
	}
	if (ranged)
	{
		counts.client_draws++;
		counts.client_bytes += bytes;
	}
}

/* HALO_GLES2_DRAW_TIMES: how long the driver's draw calls take, by program:
every 300 frames the eight programs whose indexed draws took longest, with
their draws and indices a frame (draw_times_frame, from the present) */
#define DRAW_TIME_PROGRAMS 512
static struct
{
	unsigned long draws, indices, client;
	Uint64 ns;
} draw_times[DRAW_TIME_PROGRAMS];
static int draw_timing = -1;
/* (tentative: defined with the programs, below) */
static GLuint program_vertex_shaders[LAYER_TRACKED];
static GLuint program_fragment_shaders[LAYER_TRACKED];

static void draw_times_frame(void)
{
	static unsigned long frames;
	int pass, index;
	Uint64 total = 0;
	unsigned long draws = 0;

	if (draw_timing <= 0 || ++frames % 300)
		return;
	for (index = 0; index < DRAW_TIME_PROGRAMS; index++)
	{
		total += draw_times[index].ns;
		draws += draw_times[index].draws;
	}
	platform_log("gles2 draw times: %.2f ms a frame in %lu indexed draws a frame", (double)total / 300.0e6, draws / 300);
	for (pass = 0; pass < 8; pass++)
	{
		int best = -1;

		for (index = 0; index < DRAW_TIME_PROGRAMS; index++)
		{
			if (draw_times[index].ns && (best < 0 || draw_times[index].ns > draw_times[best].ns))
				best = index;
		}
		if (best < 0)
			break;
		platform_log("gles2 draw times: program %d: %.2f ms a frame, %.1f draws (%.1f from client indices), %.0f indices, %.0f us a draw (shaders %u and %u)",
			best, (double)draw_times[best].ns / 300.0e6, (double)draw_times[best].draws / 300.0,
			(double)draw_times[best].client / 300.0, (double)draw_times[best].indices / 300.0,
			(double)draw_times[best].ns / 1000.0 / (double)draw_times[best].draws, program_vertex_shaders[best],
			program_fragment_shaders[best]);
		draw_times[best].ns = 0;
	}
	memset(draw_times, 0, sizeof(draw_times));
}

/* HALO_GLES2_DRAW_LOG=file: every indexed draw of frames 900 to 1000, a line
each: the nanoseconds its draw call took, and what was set since the draw
before it (programs, texture binds, texture parameters, uniform calls,
attribute pointers, framebuffers), whether its indices were the client's,
how many, and its program. For fitting what each kind of change costs. */
#define DRAW_LOG_ROWS 16384
static struct draw_log_row
{
	unsigned int ns, program;
	unsigned short programs, binds, parameters, uniforms, pointers, framebuffers, client;
	unsigned int indices;
} *draw_log_rows;
static unsigned long draw_log_count, draw_log_frame;
static int draw_logging = -1;

static void draw_log_add(Uint64 ns, int client, GLsizei indices)
{
	static unsigned long last[6];
	struct draw_log_row *row;

	if (draw_log_frame >= 900 && draw_log_frame < 1000 && draw_log_rows && draw_log_count < DRAW_LOG_ROWS)
	{
		row = &draw_log_rows[draw_log_count++];
		row->ns = (unsigned int)ns;
		row->program = program_current;
		row->programs = (unsigned short)(counts.programs - last[0]);
		row->binds = (unsigned short)(counts.texture_binds - last[1]);
		row->parameters = (unsigned short)(counts.texture_parameters - last[2]);
		row->uniforms = (unsigned short)(counts.uniforms - last[3]);
		row->pointers = (unsigned short)(counts.pointers - last[4]);
		row->framebuffers = (unsigned short)(counts.framebuffers - last[5]);
		row->client = (unsigned short)client;
		row->indices = (unsigned int)indices;
	}
	last[0] = counts.programs;
	last[1] = counts.texture_binds;
	last[2] = counts.texture_parameters;
	last[3] = counts.uniforms;
	last[4] = counts.pointers;
	last[5] = counts.framebuffers;
}

static void draw_log_frame_end(void)
{
	if (draw_logging <= 0)
		return;
	if (!draw_log_rows)
		draw_log_rows = calloc(DRAW_LOG_ROWS, sizeof(*draw_log_rows));
	if (++draw_log_frame == 1000 && draw_log_rows)
	{
		FILE *file = fopen(getenv("HALO_GLES2_DRAW_LOG"), "w");
		unsigned long index;

		if (file)
		{
			for (index = 0; index < draw_log_count; index++)
			{
				const struct draw_log_row *row = &draw_log_rows[index];

				fprintf(file, "%u %u %u %u %u %u %u %u %u %u%c", row->ns, row->program, row->programs, row->binds,
					row->parameters, row->uniforms, row->pointers, row->framebuffers, row->client, row->indices, 10);
			}
			fclose(file);
		}
		platform_log("gles2: %lu draws logged", draw_log_count);
	}
}

static void GL_APIENTRY layer_DrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	twin_draw_begin();
	program_ready(program_current);
	merge_stats_draw(program_current);
	counts.draws++;
	draws_total++;
	Uint64 start = 0;
	struct layer_buffer *elements;
	const void *offset;

	if (slow_checking < 0)
		slow_checking = getenv("HALO_GLES2_SLOW") != NULL;
	if (slow_checking)
	{
		halo_glFinish();
		start = SDL_GetTicksNS();
	}
	if (profiling)
		profile_report();
	if (tracing())
		trace("draw elements mode %u count %d type 0x%x program %u", (unsigned)mode, (int)count, (unsigned)type,
			program_current);
	/* (indices in a buffer with a GPU copy, the mirror's, drawn from that:
	the driver copies client indices each draw) */
	elements = buffer_get(vertex_array()->element_buffer);
	offset = indices;
	indices = buffer_pointer(vertex_array()->element_buffer, indices);
	skin_log(count, type, indices);
	border_uniforms_apply();
	position_uniforms_apply();
	if (indices && count > 0 && bones_window_needed())
	{
		unsigned long lowest, highest;

		index_range(type, indices, count, &lowest, &highest);
		bones_window_apply(lowest, highest);
	}
	if (indices && count > 0 && (vertex_array()->packed & vertex_array()->enabled))
	{
		unsigned long lowest, highest;

		normals_index_range(type, indices, count, vertex_array()->element_buffer, offset, &lowest, &highest);
		normals_apply(lowest, highest);
	}
	if (indices)
	{
		unsigned int feedback = feedback_begin();
		unsigned int experiment = experiment_begin();

		if (!(experiment & 4) && !draw_degenerate(0, count, type, indices) && !program_skipped(program_current))
		{
			unsigned int shared = shared_units_begin();

			GLsizei drawn = (experiment_bits & 128) && count > 3 ? 3 : count;

			Uint64 draw_start = 0;

			counts.index_count += (unsigned long)drawn;
			client_vertices_count(type, indices, drawn);
			if (draw_timing < 0)
			{
				draw_logging = getenv("HALO_GLES2_DRAW_LOG") != NULL;
				draw_timing = getenv("HALO_GLES2_DRAW_TIMES") != NULL || draw_logging;
			}
			if (draw_timing)
				draw_start = SDL_GetTicksNS();
			if (elements && elements->gpu)
			{
				counts.gpu_indexed++;
				real.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, elements->gpu);
				real_DrawElements(mode, drawn, type, offset);
			}
			else
			{
				real.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
				real_DrawElements(mode, drawn, type, indices);
			}
			if (draw_timing)
			{
				unsigned int slot = program_current % DRAW_TIME_PROGRAMS;

				draw_times[slot].ns += SDL_GetTicksNS() - draw_start;
				draw_times[slot].draws++;
				draw_times[slot].indices += (unsigned long)drawn;
				draw_times[slot].client += !(elements && elements->gpu);
				if (draw_logging > 0)
					draw_log_add(SDL_GetTicksNS() - draw_start, !(elements && elements->gpu), drawn);
			}
			shared_units_end(shared);
		}
		experiment_end(experiment);
		feedback_end(feedback);
	}
	if (slow_checking)
		draw_slow_check("draw elements", mode, count, type, start);
	draw_traced("draw elements", mode, count);
	draw_flush_maybe();
}

static void GL_APIENTRY layer_DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices,
	GLint base_vertex)
{
	(void)base_vertex;
	LAYER_ONCE("no base vertex");
	layer_DrawElements(mode, count, type, indices);
}

/* ---------- framebuffers */

static GLuint draw_framebuffer_seen(void)
{
	return draw_framebuffer;
}

/* the framebuffer a target names, bound for the call (ES 2 has one) */
static GLuint framebuffer_for(GLenum target)
{
	GLuint framebuffer = target == GL_READ_FRAMEBUFFER ? read_framebuffer : draw_framebuffer;

	if (framebuffer != draw_framebuffer)
		real.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	return framebuffer;
}

static void framebuffer_done(GLuint framebuffer)
{
	if (framebuffer != draw_framebuffer)
		real.BindFramebuffer(GL_FRAMEBUFFER, draw_framebuffer);
}

static void GL_APIENTRY layer_BindFramebuffer(GLenum target, GLuint framebuffer)
{
	if (target == GL_READ_FRAMEBUFFER)
	{
		read_framebuffer = framebuffer;
		return;
	}
	if (target == GL_FRAMEBUFFER)
		read_framebuffer = framebuffer;
	draw_framebuffer = framebuffer;
	counts.framebuffers++;
	merge_changed_framebuffer = 1;
	real.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
}

static void GL_APIENTRY layer_FramebufferTexture2D(GLenum target, GLenum attachment, GLenum texture_target,
	GLuint texture, GLint level)
{
	GLuint framebuffer = framebuffer_for(target);

	trace("framebuffer %u attachment 0x%x texture %u", framebuffer, (unsigned)attachment, texture);
	if (attachment == GL_COLOR_ATTACHMENT0 && framebuffer < LAYER_TRACKED)
		framebuffer_colors[framebuffer] = texture;
	if (attachment == GL_DEPTH_STENCIL_ATTACHMENT)
	{
		real.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, texture_target, texture, level);
		real.FramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, texture_target, texture, level);
	}
	else
	{
		real.FramebufferTexture2D(GL_FRAMEBUFFER, attachment, texture_target, texture, level);
	}
	framebuffer_done(framebuffer);
}

static void GL_APIENTRY layer_FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffer_target,
	GLuint renderbuffer)
{
	GLuint framebuffer = framebuffer_for(target);

	if (attachment == GL_DEPTH_STENCIL_ATTACHMENT)
	{
		real.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, renderbuffer_target, renderbuffer);
		real.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, renderbuffer_target, renderbuffer);
	}
	else
	{
		real.FramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, renderbuffer_target, renderbuffer);
	}
	framebuffer_done(framebuffer);
}

static GLenum GL_APIENTRY layer_CheckFramebufferStatus(GLenum target)
{
	GLuint framebuffer = framebuffer_for(target);
	GLenum status = real.CheckFramebufferStatus(GL_FRAMEBUFFER);

	trace("framebuffer %u status 0x%x", framebuffer, (unsigned)status);

	framebuffer_done(framebuffer);
	return status;
}

static void GL_APIENTRY layer_ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
	GLenum type, void *pixels)
{
	GLuint framebuffer = framebuffer_for(GL_READ_FRAMEBUFFER);

	real.ReadPixels(x, y, width, height, format, type, pixels);
	framebuffer_done(framebuffer);
}

static void GL_APIENTRY layer_DrawBuffers(GLsizei count, const GLenum *buffers)
{
	(void)count;
	(void)buffers;
}

static void GL_APIENTRY layer_ReadBuffer(GLenum buffer)
{
	(void)buffer;
}

static void GL_APIENTRY layer_InvalidateFramebuffer(GLenum target, GLsizei count, const GLenum *attachments)
{
	(void)target;
	if (real.DiscardFramebufferEXT && draw_framebuffer)
		real.DiscardFramebufferEXT(GL_FRAMEBUFFER, count, attachments);
}

static void GL_APIENTRY layer_RenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internal,
	GLsizei width, GLsizei height)
{
	if (samples > 1)
		LAYER_ONCE("no multisampling");
	if (internal == GL_DEPTH24_STENCIL8)
		internal = LAYER_DEPTH24_STENCIL8_OES;
	else if (internal == GL_RGBA8)
		internal = LAYER_RGBA8_OES;
	real.RenderbufferStorage(target, internal, width, height);
}

/* HALO_GLES2_SHOT=folder: the window's picture as a PPM there, every 300th
frame presented (shot0.ppm, shot1.ppm...): what the box shows, over ssh */
static void screenshot_maybe(GLint x, GLint y, GLsizei width, GLsizei height)
{
	static unsigned long frames, shots;
	const char *folder = getenv("HALO_GLES2_SHOT");
	unsigned char *pixels;
	char path[512];
	FILE *file;
	GLsizei row;

	/* (HALO_GLES2_SHOT_EVERY: another number of frames) */
	static long every = -1;

	if (every < 0)
		every = getenv("HALO_GLES2_SHOT_EVERY") ? atol(getenv("HALO_GLES2_SHOT_EVERY")) : 300;
	if (every <= 0)
		every = 300;
	if (!folder || width <= 0 || height <= 0 || ++frames % (unsigned long)every)
		return;
	pixels = malloc((size_t)width * (size_t)height * 4);
	if (!pixels)
		return;
	real.ReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	snprintf(path, sizeof(path), "%s/shot%lu.ppm", folder, shots++ % 8);
	file = fopen(path, "wb");
	if (file)
	{
		fprintf(file, "P6\n%d %d\n255\n", (int)width, (int)height);
		/* (bottom row first in GL: the file's top row first) */
		for (row = height - 1; row >= 0; row--)
		{
			const unsigned char *texel = pixels + (size_t)row * (size_t)width * 4;
			GLsizei column;

			for (column = 0; column < width; column++, texel += 4)
				fwrite(texel, 1, 3, file);
		}
		fclose(file);
		platform_log("gles2: frame %lu saved to %s", frames, path);
	}
	free(pixels);
}

/* a blit: the read framebuffer's color texture drawn as a quad into the
draw framebuffer */
static int blit_program_make(void)
{
	static const char *vertex =
		"attribute vec2 corner;\n"
		"uniform vec4 source;\n"
		"varying vec2 coordinates;\n"
		"void main()\n"
		"{\n"
		"	coordinates = source.xy + corner * source.zw;\n"
		"	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char *fragment =
		"precision mediump float;\n"
		"uniform sampler2D picture;\n"
		"varying vec2 coordinates;\n"
		"void main()\n"
		"{\n"
		"	gl_FragColor = texture2D(picture, coordinates);\n"
		"}\n";
	GLuint shaders[2], index;
	GLint linked = 0;

	if (blit_program)
		return 1;
	for (index = 0; index < 2; index++)
	{
		const char *source = index ? fragment : vertex;

		shaders[index] = real.CreateShader(index ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
		real.ShaderSource(shaders[index], 1, &source, NULL);
		real.CompileShader(shaders[index]);
	}
	blit_program = halo_glCreateProgram();
	real.AttachShader(blit_program, shaders[0]);
	real.AttachShader(blit_program, shaders[1]);
	halo_glBindAttribLocation(blit_program, 0, "corner");
	real.LinkProgram(blit_program);
	halo_glGetProgramiv(blit_program, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		LAYER_ONCE("the blit's program does not link");
		blit_program = 0;
		return 0;
	}
	blit_texture_uniform = halo_glGetUniformLocation(blit_program, "picture");
	blit_rectangle_uniform = halo_glGetUniformLocation(blit_program, "source");
	return 1;
}

static void GL_APIENTRY layer_BlitFramebuffer(GLint source_x0, GLint source_y0, GLint source_x1, GLint source_y1,
	GLint destination_x0, GLint destination_y0, GLint destination_x1, GLint destination_y1, GLbitfield mask,
	GLenum filter)
{
	static const GLfloat corners[] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
	GLint type = 0, name = 0;
	struct layer_texture *texture;
	struct layer_vertex_array *current = vertex_array();
	GLuint framebuffer;

	if (!(mask & GL_COLOR_BUFFER_BIT))
	{
		LAYER_ONCE("no depth or stencil blits");
		return;
	}
	framebuffer = framebuffer_for(GL_READ_FRAMEBUFFER);
	if (read_framebuffer)
	{
		real.GetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
		real.GetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
	}
	framebuffer_done(framebuffer);
	texture = texture_get((GLuint)name);
	/* a blit into the framebuffer it reads (the renderer's first present
	makes its back buffer's framebuffer with GL_FRAMEBUFFER, which leaves it
	the draw framebuffer too): ES 3 copies over itself, here it would draw
	from the texture it draws into, which the GPU does not survive */
	if (read_framebuffer == draw_framebuffer)
	{
		LAYER_ONCE("a blit into the framebuffer it reads (%u): left out", read_framebuffer);
		return;
	}
	if (type != GL_TEXTURE || !texture || !texture->width || !texture->height)
	{
		LAYER_ONCE("a blit from something not a texture (framebuffer %u)", read_framebuffer);
		return;
	}
	if (!blit_program_make())
		return;

	/* everything the renderer has set is set again after (xgpu.h) */
	halo_glDisable(GL_BLEND);
	halo_glDisable(GL_DEPTH_TEST);
	halo_glDisable(GL_STENCIL_TEST);
	halo_glDisable(GL_CULL_FACE);
	halo_glDisable(GL_SCISSOR_TEST);
	halo_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	halo_glViewport(destination_x0 < destination_x1 ? destination_x0 : destination_x1,
		destination_y0 < destination_y1 ? destination_y0 : destination_y1,
		abs(destination_x1 - destination_x0), abs(destination_y1 - destination_y0));
	real.UseProgram(blit_program);
	real.ActiveTexture(GL_TEXTURE0);
	real.BindTexture(GL_TEXTURE_2D, (GLuint)name);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	texture->applied_min = texture->applied_mag = 0;
	halo_glUniform1i(blit_texture_uniform, 0);
	{
		/* (a flipped rectangle, either way, flips the picture) */
		GLfloat source[4];

		source[0] = (GLfloat)source_x0 / (GLfloat)texture->width;
		source[1] = (GLfloat)source_y0 / (GLfloat)texture->height;
		source[2] = (GLfloat)(source_x1 - source_x0) / (GLfloat)texture->width;
		source[3] = (GLfloat)(source_y1 - source_y0) / (GLfloat)texture->height;
		if (destination_x1 < destination_x0)
		{
			source[0] += source[2];
			source[2] = -source[2];
		}
		if (destination_y1 < destination_y0)
		{
			source[1] += source[3];
			source[3] = -source[3];
		}
		halo_glUniform4fv(blit_rectangle_uniform, 1, source);
	}
	/* only attribute 0: the GPU reads every enabled one for each vertex,
	wherever its buffer and offset point now */
	{
		GLuint index;

		for (index = 1; index < (GLuint)attribute_maximum; index++)
		{
			if (current->enabled & (1u << index))
				real.DisableVertexAttribArray(index);
		}
	}
	real.BindBuffer(GL_ARRAY_BUFFER, 0);
	real.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
	if (!(current->enabled & 1))
		real.EnableVertexAttribArray(0);
	trace("blit %d,%d-%d,%d of framebuffer %u (texture %d) to %d,%d-%d,%d of %u", source_x0, source_y0, source_x1,
		source_y1, read_framebuffer, name, destination_x0, destination_y0, destination_x1, destination_y1,
		draw_framebuffer);
	real_DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	/* the vertex array's attribute 0, the unit's texture and the buffer
	back as they were; the rest the renderer sets again */
	if (current->enabled & 1)
		attribute_apply(0, &current->attributes[0]);
	else
		real.DisableVertexAttribArray(0);
	{
		GLuint index;

		for (index = 1; index < (GLuint)attribute_maximum; index++)
		{
			if (current->enabled & (1u << index))
				real.EnableVertexAttribArray(index);
		}
	}
	real.BindTexture(GL_TEXTURE_2D, unit_textures[0][0] ? unit_textures[0][0] : texture_white);
	if (active_unit != 0)
		real.ActiveTexture(GL_TEXTURE0 + active_unit);
	xgpu_gl_state_invalidate();
	if (!draw_framebuffer)
	{
		/* the first frames' times in the log, presented to presented */
		static Uint64 last;
		static int timed;
		Uint64 now = SDL_GetTicks();

		if (profiling)
			profile_report();
		static Uint64 period_start, finish_ms;

		/* (HALO_GLES2_FRAME_SPLIT: the GPU's share of each frame, as the time
		a glFinish waits at its end) */
		static int frame_split = -1;

		if (frame_split < 0)
			frame_split = getenv("HALO_GLES2_FRAME_SPLIT") != NULL;
		if (frame_split)
		{
			Uint64 before = SDL_GetTicks();

			halo_glFinish();
			finish_ms += SDL_GetTicks() - before;
			now = SDL_GetTicks();
		}

		if (timed++ < 30)
			platform_log("gles2: frame %d presented, %llu ms after the last", timed, (unsigned long long)(now - last));
		/* and every 300 frames after, their average */
		if (timed == 30)
			period_start = now;
		else if (timed > 30 && !((timed - 30) % 300))
		{
			platform_log("gles2: frames %d to %d took %.1f ms each; a frame's calls: %lu draws, %lu programs, "
				"%lu texture binds, %lu sampler states, %lu uniforms, %lu attribute pointers, %lu framebuffers, %lu drawn from GPU indices; %lu links and %lu compiles in all",
				timed - 300, timed, (double)(now - period_start) / 300.0, counts.draws / 300,
				counts.programs / 300, counts.texture_binds / 300, counts.texture_parameters / 300,
				counts.uniforms / 300, counts.pointers / 300, counts.framebuffers / 300,
				counts.gpu_indexed / 300, counts.links,
				counts.compiles);
			/* (where the memory is: the C heap, its large blocks, and the
			layer's own copies of the renderer's buffers) */
			{
				/* (glibc's, declared here: the port's headers stand in front
				of malloc.h) */
				struct layer_mallinfo { int arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
				extern struct layer_mallinfo mallinfo(void);
				struct layer_mallinfo heap = mallinfo();
				unsigned long copies = 0, copied_bytes = 0, aliased = 0, with_gpu = 0;
				GLuint name;

				for (name = 1; name < LAYER_TRACKED; name++)
				{
					if (!buffers[name].data)
						continue;
					if (buffers[name].aliased)
						aliased++;
					else
					{
						copies++;
						copied_bytes += buffers[name].size > 0 ? (unsigned long)buffers[name].size : 0;
					}
					with_gpu += buffers[name].gpu != 0;
				}
				platform_log("gles2: memory: heap %d KB in use of %d, large blocks %d KB in %d; the layer's buffers: "
					"%lu copies of %lu KB, %lu aliased, %lu with a GPU copy", heap.uordblks / 1024, heap.arena / 1024,
					heap.hblkhd / 1024, heap.hblks, copies, copied_bytes / 1024, aliased, with_gpu);
				platform_log("gles2: a frame's vertices: %lu draws from client arrays (%lu KB the driver copies), %lu indices",
					counts.client_draws / 300, counts.client_bytes / 300 / 1024, counts.index_count / 300);
			}
			if (finish_ms)
				platform_log("gles2: of which %.1f ms waiting for the GPU at the frame's end", (double)finish_ms / 300.0);
			if (merge_stats_on > 0)
			{
				platform_log("gles2: a frame's draws by what changed first: framebuffer %lu, program %lu, textures %lu, "
					"uniforms %lu, vertices only %lu, nothing %lu; %lu constants written, %lu to the value they had",
					merge_stats.framebuffers / 300, merge_stats.programs / 300, merge_stats.textures / 300,
					merge_stats.uniforms / 300, merge_stats.pointers / 300, merge_stats.nothing / 300,
					merge_stats.uniform_writes / 300, merge_stats.uniform_repeats / 300);
				memset(&merge_stats, 0, sizeof(merge_stats));
			}
			finish_ms = 0;
			memset(&counts, 0, sizeof(counts));
			period_start = now;
		}
		perf_ab(timed, now - last);
	draw_times_frame();
	draw_log_frame_end();
	normals_frame();
	twin_frame();
		last = now;
		screenshot_maybe(destination_x0 < destination_x1 ? destination_x0 : destination_x1,
			destination_y0 < destination_y1 ? destination_y0 : destination_y1, abs(destination_x1 - destination_x0),
			abs(destination_y1 - destination_y0));
	}
}

/* HALO_PERF_AB="global[+global...][=a/b] ...": the game frozen (game_speed 0) at frame
HALO_PERF_AB_AT (1500), and from 30 frames on, each script global in turn
at a (false unless given) and at b (true) for 12 frames each, three times
over, and its average frame at each in the log: what a rendering pass costs
on the same picture. The first 4 frames of each 12 are left out (the change
settling). A global named layer_experiment is the layer's
HALO_GLES2_EXPERIMENT bits on every program (layer_experiment=4/0, no draws
against all). HALO_PERF_AB_OFF="global ...": those false from the freeze on,
for what is left once they are. */
extern unsigned char hs_compile_and_evaluate(char const *expression);

#define PERF_AB_HALF 12
#define PERF_AB_SETTLE 4
#define PERF_AB_HALVES 6

static void perf_ab(int frame, Uint64 took)
{
	static char *list;
	static char *items[64];
	static int item_count = -1;
	static Uint64 sums[2];
	static unsigned long draws[2], draws_last;
	static int frames[2];
	static int start;
	int position, half, item;
	char name[512], values[2][24], command[160], *global, *next;

	if (item_count < 0)
	{
		const char *value = getenv("HALO_PERF_AB");
		char *token;

		item_count = 0;
		if (!value)
			return;
		start = getenv("HALO_PERF_AB_AT") ? atoi(getenv("HALO_PERF_AB_AT")) : 1500;
		list = strdup(value);
		for (token = strtok(list, " "); token && item_count < 64; token = strtok(NULL, " "))
			items[item_count++] = token;
	}
	if (!item_count)
		return;
	if (frame == start)
	{
		platform_log("perf ab: the game frozen at frame %d", frame);
		hs_compile_and_evaluate("(game_speed 0)");
		if (getenv("HALO_PERF_AB_OFF"))
		{
			char *off = strdup(getenv("HALO_PERF_AB_OFF")), *token;

			for (token = strtok(off, " "); token; token = strtok(NULL, " "))
			{
				snprintf(command, sizeof(command), "(set %s false)", token);
				hs_compile_and_evaluate(command);
			}
			free(off);
		}
	}
	if (frame < start + 30)
		return;
	position = (frame - start - 30) % PERF_AB_HALF;
	half = (frame - start - 30) / PERF_AB_HALF;
	item = half / PERF_AB_HALVES;
	if (position >= PERF_AB_SETTLE && item < item_count)
	{
		sums[half & 1] += took;
		draws[half & 1] += draws_total - draws_last;
		frames[half & 1]++;
	}
	draws_last = draws_total;
	if (position)
		return;
	/* (a half begins: an item's end logged, the next half's value set) */
	if (half && !(half % PERF_AB_HALVES) && item <= item_count)
	{
		platform_log("perf ab: %s: %.1f ms at the first, %.1f ms at the second (%lu and %lu draws a frame)",
			items[item - 1], frames[0] ? (double)sums[0] / frames[0] : 0.0,
			frames[1] ? (double)sums[1] / frames[1] : 0.0, frames[0] ? draws[0] / frames[0] : 0,
			frames[1] ? draws[1] / frames[1] : 0);
		sums[0] = sums[1] = 0;
		draws[0] = draws[1] = 0;
		frames[0] = frames[1] = 0;
	}
	if (item >= item_count)
		return;
	snprintf(name, sizeof(name), "%s", items[item]);
	snprintf(values[0], sizeof(values[0]), "false");
	snprintf(values[1], sizeof(values[1]), "true");
	{
		char *equals = strchr(name, '=');
		char *slash = equals ? strchr(equals, '/') : NULL;

		if (equals && slash)
		{
			*equals = *slash = 0;
			snprintf(values[0], sizeof(values[0]), "%s", equals + 1);
			snprintf(values[1], sizeof(values[1]), "%s", slash + 1);
		}
	}
	/* (an even count of halves: each ends at the second, as it was) */
	for (global = name; global; global = next)
	{
		next = strchr(global, '+');
		if (next)
			*next++ = 0;
		if (!strcmp(global, "layer_experiment"))
		{
			experiment_program = 0;
			experiment_bits = (unsigned int)atoi(values[half & 1]);
			continue;
		}
		snprintf(command, sizeof(command), "(set %s %s)", global, values[half & 1]);
		hs_compile_and_evaluate(command);
	}
}

/* ---------- queries, buffer copies */

static void GL_APIENTRY layer_GenQueries(GLsizei count, GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
		names[index] = (GLuint)index + 1;
}

static void GL_APIENTRY layer_BeginQuery(GLenum target, GLuint name)
{
	(void)target;
	(void)name;
}

static void GL_APIENTRY layer_EndQuery(GLenum target)
{
	(void)target;
}

static void GL_APIENTRY layer_GetQueryObjectuiv(GLuint name, GLenum pname, GLuint *value)
{
	(void)name;
	/* available, and everything visible (lens flares at full strength) */
	*value = pname == GL_QUERY_RESULT_AVAILABLE ? 1 : 1000000;
}

static void GL_APIENTRY layer_CopyBufferSubData(GLenum read_target, GLenum write_target, GLintptr read_offset,
	GLintptr write_offset, GLsizeiptr size)
{
	GLuint *read_binding = buffer_binding(read_target), *write_binding = buffer_binding(write_target);
	struct layer_buffer *from = read_binding ? buffer_get(*read_binding) : NULL;
	struct layer_buffer *to = write_binding ? buffer_get(*write_binding) : NULL;

	if (!from || !to || !from->data || !to->data || read_offset < 0 || write_offset < 0 || size < 0 ||
		read_offset + size > from->size || write_offset + size > to->size)
	{
		return;
	}
	memmove(to->data + write_offset, from->data + read_offset, (size_t)size);
	buffer_pages_uploaded(to, write_offset, size);
}

static void GL_APIENTRY layer_BindBufferBase(GLenum target, GLuint index, GLuint buffer)
{
	(void)target; (void)index; (void)buffer;
}

static void GL_APIENTRY layer_BindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset,
	GLsizeiptr size)
{
	(void)target; (void)index; (void)buffer; (void)offset; (void)size;
}

/* ---------- state queries */

static void GL_APIENTRY layer_GetIntegerv(GLenum name, GLint *value)
{
	switch (name)
	{
	/* ES 3.0 is what the renderer is told it has (gl_initialize) */
	case GL_MAJOR_VERSION: *value = 3; return;
	case GL_MINOR_VERSION: *value = 0; return;
	case GL_VERTEX_ARRAY_BINDING: *value = (GLint)vertex_array_current; return;
	case GL_SAMPLER_BINDING: *value = active_unit < LAYER_TEXTURE_UNITS ? (GLint)unit_samplers[active_unit] : 0; return;
	case GL_DRAW_FRAMEBUFFER_BINDING: *value = (GLint)draw_framebuffer; return;
	case GL_READ_FRAMEBUFFER_BINDING: *value = (GLint)read_framebuffer; return;
	case GL_MAX_SAMPLES: *value = 0; return;
	case GL_TEXTURE_BINDING_2D_ARRAY:
	case GL_TEXTURE_BINDING_3D:
	case GL_NUM_EXTENSIONS:
		*value = 0;
		return;
	default:
		real.GetIntegerv(name, value);
		return;
	}
}

/* ---------- shaders: GLSL ES 3.00 as GLSL ES 1.00 */

struct layer_shader
{
	GLenum type;
	char *source;
	/* a vertex shader's attributes and their locations (layout(location)) */
	int attribute_count;
	char attribute_names[LAYER_ATTRIBUTES][32];
	GLint attribute_locations[LAYER_ATTRIBUTES];
	/* compiled only when a program with it is linked from its source, not
	loaded as a binary (program_binaries) */
	int deferred;
	/* a skinned shader's node indices: the attribute (v<n>) and the constant
	(c[<k>].w) its a0 is taken from, and its weights' attribute (the first
	node's in x, both's sum in w), -1 for none (bones_window_apply) */
	int node_attribute, node_scale, weight_attribute;
};

static struct layer_shader shaders[LAYER_TRACKED];
/* the vertex shader each program has */
static GLuint program_vertex_shaders[LAYER_TRACKED];

/* whether a program's vertex shader reads relative constants, c[a0 + n] */
static int program_relative(GLuint program)
{
	GLuint vertex = program && program < LAYER_TRACKED ? program_vertex_shaders[program] : 0;

	return vertex && shaders[vertex].source && strstr(shaders[vertex].source, "(a0 +") != NULL;
}

/* HALO_GLES2_SKIP_RELATIVE: the draws whose vertex shader reads relative
constants (c[a0 + n], the skinned models) left out, to tell what they draw */
static int program_skipped(GLuint program)
{
	static int skip = -1;
	GLuint vertex;

	if (skip < 0)
		skip = getenv("HALO_GLES2_SKIP_RELATIVE") != NULL;
	if (!skip || !program || program >= LAYER_TRACKED)
		return 0;
	vertex = program_vertex_shaders[program];
	return vertex && shaders[vertex].source && strstr(shaders[vertex].source, "(a0 +");
}
static GLuint program_fragment_shaders[LAYER_TRACKED];

struct text
{
	char *buffer;
	size_t length, size;
};

static void text_append(struct text *text, const char *data, size_t length)
{
	if (text->length + length + 1 > text->size)
	{
		size_t size = text->size ? text->size : 4096;
		char *buffer;

		while (text->length + length + 1 > size)
			size *= 2;
		buffer = realloc(text->buffer, size);
		if (!buffer)
			return;
		text->buffer = buffer;
		text->size = size;
	}
	memcpy(text->buffer + text->length, data, length);
	text->length += length;
	text->buffer[text->length] = 0;
}

static void text_add(struct text *text, const char *string)
{
	text_append(text, string, strlen(string));
}

static int starts_with(const char *text, const char *prefix)
{
	return !strncmp(text, prefix, strlen(prefix));
}

static int is_name_character(char character)
{
	return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
		(character >= '0' && character <= '9') || character == '_';
}

/* source with every whole-word from replaced by to */
static void replace_words(struct text *text, const char *source, size_t length, const char *const *pairs)
{
	size_t index = 0;

	while (index < length)
	{
		const char *const *pair;
		int replaced = 0;

		if (!index || !is_name_character(source[index - 1]))
		{
			for (pair = pairs; *pair; pair += 2)
			{
				size_t word = strlen(pair[0]);

				if (index + word <= length && !strncmp(source + index, pair[0], word) &&
					(index + word == length || !is_name_character(source[index + word])))
				{
					text_add(text, pair[1]);
					index += word;
					replaced = 1;
					break;
				}
			}
		}
		if (!replaced)
		{
			text_append(text, source + index, 1);
			index++;
		}
	}
}

/* "TYPE NAME;" (after the qualifier): NAME */
static void declaration_name(const char *declaration, char *name, size_t size)
{
	const char *end = strchr(declaration, ';'), *start;
	size_t length;

	name[0] = 0;
	if (!end)
		return;
	while (end > declaration && (end[-1] == ' ' || end[-1] == ']' || end[-1] == '[' ||
		(end[-1] >= '0' && end[-1] <= '9' && strchr(declaration, '['))))
	{
		if (end[-1] == '[')
		{
			end--;
			break;
		}
		end--;
	}
	start = end;
	while (start > declaration && is_name_character(start[-1]))
		start--;
	length = (size_t)(end - start);
	if (length >= size)
		length = size - 1;
	memcpy(name, start, length);
	name[length] = 0;
}

/* (texture() with a bias samples without it: Vivante's GPU hangs on some
draws that sample with one, and the renderer's biases are 0 unless a
texture asks) */
static const char fragment_helpers[] =
	"vec4 texture(sampler2D s, vec2 c) { return texture2D(s, c); }\n"
	"vec4 texture(sampler2D s, vec2 c, float b) { return texture2D(s, c); }\n"
	"vec4 texture(sampler2D s, vec3 c) { return texture2D(s, c.xy); }\n"
	"vec4 texture(sampler2D s, vec3 c, float b) { return texture2D(s, c.xy); }\n"
	"vec4 texture(samplerCube s, vec3 c) { return textureCube(s, c); }\n"
	"vec4 texture(samplerCube s, vec3 c, float b) { return textureCube(s, c); }\n"
	"vec4 textureLod(sampler2D s, vec2 c, float l) { return texture2D(s, c); }\n"
	"vec4 textureLod(samplerCube s, vec3 c, float l) { return textureCube(s, c); }\n";
/* D3DTADDRESS_BORDER, which ES 2 has not: each unit's 2D lookups give its
border color where they fall outside the texture on an axis that has one
(the motion tracker's sweep is drawn with its coordinates past the edges,
and the border's alpha fades the blips under it). The pixel shaders'
texture(texN, (...)) become layer_sampleN (text_replacements); the
uniforms are border_uniforms_apply's, all 0 where no unit has a border */
static const char border_helpers_head[] =
	"uniform vec4 layer_border_color[4];\n"
	"uniform vec4 layer_border_axes[4];\n";
static const char border_helper_unit[] =
	"vec4 layer_sample%d(sampler2D s, vec2 c, float b)\n"
	"{\n"
	"\tvec2 o = step(vec2(0.5), abs(c - 0.5)) * layer_border_axes[%d].xy;\n"
	"\treturn mix(texture2D(s, c), layer_border_color[%d], max(o.x, o.y));\n"
	"}\n"
	"vec4 layer_sample%d(sampler2D s, vec2 c) { return layer_sample%d(s, c, 0.0); }\n"
	"vec4 layer_sample%d(sampler2D s, vec3 c, float b) { return texture2D(s, c.xy); }\n"
	"vec4 layer_sample%d(sampler2D s, vec3 c) { return texture2D(s, c.xy); }\n"
	"vec4 layer_sample%d(samplerCube s, vec3 c, float b) { return textureCube(s, c); }\n"
	"vec4 layer_sample%d(samplerCube s, vec3 c) { return textureCube(s, c); }\n";
static const char *const border_replacements[] = {
	"texture(tex0, (", "layer_sample0(tex0, (",
	"texture(tex1, (", "layer_sample1(tex1, (",
	"texture(tex2, (", "layer_sample2(tex2, (",
	"texture(tex3, (", "layer_sample3(tex3, (",
	NULL,
};
static const char vertex_helpers[] =
	"vec4 texture(sampler2D s, vec2 c) { return texture2DLod(s, c, 0.0); }\n"
	"vec4 texture(samplerCube s, vec3 c) { return textureCubeLod(s, c, 0.0); }\n"
	"vec4 textureLod(sampler2D s, vec2 c, float l) { return texture2DLod(s, c, l); }\n";
static const char common_helpers[] =
	"float round(float x) { return floor(x + 0.5); }\n"
	"vec2 round(vec2 x) { return floor(x + 0.5); }\n"
	"vec3 round(vec3 x) { return floor(x + 0.5); }\n"
	"vec4 round(vec4 x) { return floor(x + 0.5); }\n"
	"float trunc(float x) { return x < 0.0 ? -floor(-x) : floor(x); }\n"
	"int clamp_int(int x, int low, int high) { return x < low ? low : (x > high ? high : x); }\n";

/* text the generators write that ES 1.00 has under another name, or not at
all (its built-in functions cannot be overloaded: clamp is float's only) */
static const char *const text_replacements[] = {
	/* the vertex shader's relative constants, c[clamp(a0 + n, 0, 191)] */
	"[clamp(a0", "[clamp_int(a0",
	/* c[], the Xbox's 192 vertex constants, as 128: an array read with a
	relative index must fit the box's 168 vectors with the shader's other
	uniforms and the literals its compiler keeps there, or Vivante's compiler
	gives it half its size and the rest read 0 (a skinned model's nodes past
	the eighth); 164 does not link with the larger shaders. 128 holds 22
	nodes from c[60]. The shaders name none past c[59] themselves; the
	renderer's uploads past c[127] are dropped */
	"uniform vec4 c[192];", "uniform vec4 c[128];",
	", 0, 191)]", ", 0, 127)]",
	/* the pixel shader's exact texel of stage 0's texture (nv2a_psh.c: a
	threshold read where the stage samples), which ES 1.00 has neither
	textureSize nor texelFetch for: the same place, sampled */
	"ivec2 size = textureSize(tex0, 0);", "",
	"ivec2 pixel = clamp(ivec2(floor(uv * vec2(size))), ivec2(0), size - 1);", "",
	"texelFetch(tex0, pixel, 0)", "texture2D(tex0, uv)",
	/* the combiners' mux on r0.a's lowest bit (nv2a_psh.c, 0.9.0): ES 1.00
	has no integer & (a "reserved operator"), so the bit is the 8-bit
	value's oddness */
	"(int(r0.a * 255.0 + 0.5) & 1) != 0", "mod(floor(r0.a * 255.0 + 0.5), 2.0) >= 0.5",
	NULL,
};

/* source with each of pairs' text replaced wherever it is */
static void replace_text(struct text *text, const char *source, const char *const *pairs)
{
	while (*source)
	{
		const char *const *pair;
		int replaced = 0;

		for (pair = pairs; *pair; pair += 2)
		{
			size_t length = strlen(pair[0]);

			if (!strncmp(source, pair[0], length))
			{
				text_add(text, pair[1]);
				source += length;
				replaced = 1;
				break;
			}
		}
		if (!replaced)
			text_append(text, source++, 1);
	}
}

/* the whole-word appearances of word in text */
static int word_count(const char *text, const char *word)
{
	size_t length = strlen(word);
	const char *at = text;
	int count = 0;

	while ((at = strstr(at, word)) != NULL)
	{
		if ((at == text || !is_name_character(at[-1])) && !is_name_character(at[length]))
			count++;
		at += length;
	}
	return count;
}

/* The vertex shader's instructions, as nv2a_vsh.c writes them, copy each
operand into A, B and C and compute mac from them:

	A = v6.xxxx;
	B = c[clamp_int(a0 + 60, 0, 191)].xyzw;
	C = v6.xyzw;
	mac = A * B;
	r3.xyzw = mac.xyzw;

Vivante's compiler keeps the copies, and a skinned model's shader (its
relative constants, c[a0 + n], more than once) then fails to link. Folded,
each instruction computes from its operands into a temporary of its own:

	vec4 m2 = (v6.xxxx) * (c[clamp_int(a0 + 60, 0, 191)].xyzw);
	r3.xyzw = m2.xyzw;

An instruction of any other shape (the special function unit's, ilu) is
left as it was. */

/* "\tTARGET = EXPRESSION;": TARGET and EXPRESSION, or 0 */
static int assignment_parse(const char *line, size_t length, char *target, size_t target_size, const char **expression,
	size_t *expression_length)
{
	const char *equals;
	size_t name;

	if (length < 6 || line[0] != '\t' || line[1] == '\t' || line[length - 1] != ';')
		return 0;
	equals = strstr(line, " = ");
	if (!equals || equals >= line + length)
		return 0;
	name = (size_t)(equals - (line + 1));
	if (!name || name >= target_size)
		return 0;
	memcpy(target, line + 1, name);
	target[name] = 0;
	*expression = equals + 3;
	*expression_length = (size_t)(line + length - 1 - *expression);
	return 1;
}

static int is_block_start(const char *line, size_t length)
{
	return length > 6 && line[0] == '\t' && line[1] == '/' && line[2] == '*' && line[3] == ' ' && line[4] >= '0' &&
		line[4] <= '9' && line[length - 2] == '*' && line[length - 1] == '/';
}

/* the text with the whole words of a pair replaced (pairs: word, then
replacement, NULL-ended) */
static void words_into(struct text *text, const char *source, size_t length, const char *const *pairs)
{
	replace_words(text, source, length, pairs);
}

static int mentions_word(const char *text, size_t length, const char *word)
{
	size_t size = strlen(word), index;

	for (index = 0; index + size <= length; index++)
	{
		if (!strncmp(text + index, word, size) && (!index || !is_name_character(text[index - 1])) &&
			(index + size == length || !is_name_character(text[index + size])))
		{
			return 1;
		}
	}
	return 0;
}

static void operands_fold(struct text *source)
{
	struct text result = { 0 };
	const char *line = source->buffer;
	int folded = 0;

	while (line && *line)
	{
		const char *end = strchr(line, '\n');
		size_t length = end ? (size_t)(end - line) : strlen(line);

		if (is_block_start(line, length))
		{
			/* the block's lines, to the next block or the first that is not an
			assignment */
			const char *block = line, *cursor = end ? end + 1 : line + length;
			char operands[3][160];
			int have[3] = { 0, 0, 0 }, simple = 1, writes = 0;
			const char *mac = NULL, *write_lines[8];
			size_t mac_length = 0, write_lengths[8];
			const char *block_end;

			while (*cursor)
			{
				const char *next = strchr(cursor, '\n');
				size_t size = next ? (size_t)(next - cursor) : strlen(cursor);
				char target[64];
				const char *expression;
				size_t expression_length;

				if (is_block_start(cursor, size))
					break;
				if (!assignment_parse(cursor, size, target, sizeof(target), &expression, &expression_length))
				{
					/* (a line of another shape that reads what the block
					computed, an if's write: the block stays as it was) */
					if (mentions_word(cursor, size, "mac") || mentions_word(cursor, size, "ilu") ||
						mentions_word(cursor, size, "A") || mentions_word(cursor, size, "B") ||
						mentions_word(cursor, size, "C"))
					{
						simple = 0;
					}
					break;
				}
				if (!mac && (!strcmp(target, "A") || !strcmp(target, "B") || !strcmp(target, "C")) &&
					expression_length < sizeof(operands[0]))
				{
					int which = target[0] - 'A';

					memcpy(operands[which], expression, expression_length);
					operands[which][expression_length] = 0;
					have[which] = 1;
				}
				else if (!mac && !strcmp(target, "mac"))
				{
					mac = expression;
					mac_length = expression_length;
					if (mentions_word(mac, mac_length, "ilu"))
						simple = 0;
				}
				else if (mac && writes < 8 && mentions_word(expression, expression_length, "mac") &&
					!mentions_word(expression, expression_length, "A") &&
					!mentions_word(expression, expression_length, "B") &&
					!mentions_word(expression, expression_length, "C") &&
					!mentions_word(expression, expression_length, "ilu"))
				{
					write_lines[writes] = cursor;
					write_lengths[writes++] = size;
				}
				else
				{
					simple = 0;
					break;
				}
				cursor = next ? next + 1 : cursor + size;
			}
			block_end = cursor;
			if (simple && mac && writes)
			{
				char wrapped[3][168];
				const char *pairs[7];
				int index, count = 0;

				for (index = 0; index < 3; index++)
				{
					if (!have[index])
						continue;
					snprintf(wrapped[index], sizeof(wrapped[index]), "(%s)", operands[index]);
					pairs[count * 2] = index == 0 ? "A" : index == 1 ? "B" : "C";
					pairs[count * 2 + 1] = wrapped[index];
					count++;
				}
				pairs[count * 2] = NULL;
				char name[16];
				const char *to_name[3];

				/* (a name of its own for each, in the function's scope: Vivante's
				compiler draws wrong with a scope per instruction, each with its
				own m) */
				snprintf(name, sizeof(name), "m%d", folded++);
				to_name[0] = "mac";
				to_name[1] = name;
				to_name[2] = NULL;
				text_append(&result, block, length);
				text_add(&result, "\n\tvec4 ");
				text_add(&result, name);
				text_add(&result, " = ");
				words_into(&result, mac, mac_length, pairs);
				text_add(&result, ";\n");
				for (index = 0; index < writes; index++)
				{
					words_into(&result, write_lines[index], write_lengths[index], to_name);
					text_add(&result, "\n");
				}
			}
			else
			{
				/* as it was, to where the block's assignments stopped */
				text_append(&result, block, (size_t)(block_end - block));
			}
			line = block_end;
			continue;
		}
		text_append(&result, line, length + (end ? 1 : 0));
		line = end ? end + 1 : line + length;
	}
	free(source->buffer);
	*source = result;
}

/* The vertex shaders' constants, c[], are the Xbox's 192, and the box has
168 vectors for a shader's uniforms and the literals its compiler keeps
there. A shader names a few outright (c[0] to c[22], c[58], c[59], the
odd one past c[180]); Vivante's compiler keeps an array up to the highest
of them, or whole where it is read with a relative index, and links
nothing that holds more than the box has. So each constant a shader names
is a uniform of its own (c_7), and only a skinned model's shader (relative
constants, c[a0 + n]) has an array: its nodes' matrices, three a node from
c[60], as c_bones[108] (36 nodes, to c[167]; 132, to c[191], does not
link with the larger shaders), which its relative reads index and which
holds whatever of c[60] to c[167] it names too. layer_Uniform4fv sends the
renderer's uploads of c[] where they now go. */
#define BONES_FIRST 60
#define BONES_COUNT 108

static void constants_split(struct text *source, int relative)
{
	struct text result = { 0 }, declarations = { 0 };
	unsigned char named[192];
	const char *at = source->buffer, *declaration;
	int index;

	memset(named, 0, sizeof(named));
	while (at && *at)
	{
		const char *found = strstr(at, "c[");
		char *end;
		long number;

		if (!found)
		{
			text_add(&result, at);
			break;
		}
		text_append(&result, at, (size_t)(found - at));
		if ((found != source->buffer && is_name_character(found[-1])) ||
			(found - source->buffer >= 5 && !strncmp(found - 5, "vec4 ", 5)))
		{
			text_append(&result, found, 2);
			at = found + 2;
			continue;
		}
		if (relative && !strncmp(found, "c[clamp_int(a0 + ", 17))
		{
			/* c[clamp_int(a0 + K, 0, 127)]: the block, from K - 60 */
			long offset = strtol(found + 17, &end, 10);
			const char *close = strstr(end, ")]");
			char text[96];

			if (!close)
			{
				text_append(&result, found, 2);
				at = found + 2;
				continue;
			}
			snprintf(text, sizeof(text), "c_bones[clamp_int(a0 + %ld, 0, %d)]", offset - BONES_FIRST, BONES_COUNT - 1);
			text_add(&result, text);
			at = close + 2;
			continue;
		}
		number = strtol(found + 2, &end, 10);
		if (end != found + 2 && *end == ']' && number >= 0 && number < 192)
		{
			char text[32];

			if (relative && number >= BONES_FIRST && number - BONES_FIRST < BONES_COUNT)
			{
				snprintf(text, sizeof(text), "c_bones[%ld]", number - BONES_FIRST);
			}
			else
			{
				snprintf(text, sizeof(text), "c_%ld", number);
				named[number] = 1;
			}
			text_add(&result, text);
			at = end + 1;
			continue;
		}
		text_append(&result, found, 2);
		at = found + 2;
	}
	/* c[]'s declaration as theirs */
	for (index = 0; index < 192; index++)
	{
		if (named[index])
		{
			char text[40];

			snprintf(text, sizeof(text), "uniform vec4 c_%d;\n", index);
			text_add(&declarations, text);
		}
	}
	if (relative)
	{
		char text[48];

		snprintf(text, sizeof(text), "uniform vec4 c_bones[%d];\n", BONES_COUNT);
		text_add(&declarations, text);
	}
	declaration = result.buffer ? strstr(result.buffer, "uniform vec4 c[128];\n") : NULL;
	if (declaration)
	{
		struct text final = { 0 };
		size_t before = (size_t)(declaration - result.buffer);

		text_append(&final, result.buffer, before);
		if (declarations.buffer)
			text_add(&final, declarations.buffer);
		text_add(&final, declaration + strlen("uniform vec4 c[128];\n"));
		free(result.buffer);
		result = final;
	}
	free(declarations.buffer);
	free(source->buffer);
	*source = result;
}

/* A vertex shader that reads relative constants (c[a0 + n]: a skinned
model's bones, three to a node from c[60]) has its c[] sized by Vivante's
compiler to the highest constant it names outright, so the nodes past it
would read 0 and their vertices fall to a point. Naming the last, c[127],
where it is never taken keeps the whole array (text_replacements sizes it
to fit). */
static void constants_keep(struct text *source)
{
	const char *end;
	size_t at;
	struct text kept = { 0 };

	if (!source->buffer || !strstr(source->buffer, "(a0 +"))
		return;
	end = strrchr(source->buffer, '}');
	if (!end)
		return;
	at = (size_t)(end - source->buffer);
	text_append(&kept, source->buffer, at);
	/* (as c[] is written then: constants_split makes it c_bones[107]) */
	text_add(&kept, "\tif (point_size < -1.0e30) gl_Position = c[167];\n");
	text_add(&kept, end);
	free(source->buffer);
	*source = kept;
}

/* The vertex shader declares the Xbox's 16 registers (vN_in, or vN_packed)
and copies each into vN; the box has 10 attributes, and its compiler counts
a copy nobody reads. A register whose vN is read nowhere else is no
attribute here: its copy takes the register's default instead. */
static void attributes_prune(struct layer_shader *shader, struct text *source)
{
	int reg;

	for (reg = 0; reg < 16; reg++)
	{
		static const char *const suffixes[] = { "_in", "_packed" };
		char local[8], attribute[16], declaration[48];
		const char *at;
		struct text pruned = { 0 };
		const char *pairs[5];
		size_t kind;
		int index;

		snprintf(local, sizeof(local), "v%d", reg);
		if (word_count(source->buffer, local) > 1)
			continue;
		for (kind = 0; kind < 2; kind++)
		{
			snprintf(attribute, sizeof(attribute), "v%d%s", reg, suffixes[kind]);
			if (!word_count(source->buffer, attribute))
				continue;
			snprintf(declaration, sizeof(declaration), "attribute vec4 %s;\n", attribute);
			at = strstr(source->buffer, declaration);
			if (!at)
				continue;
			/* (the declaration, then the name as a whole word: v5_in is not
			v15_in) */
			pairs[0] = declaration;
			pairs[1] = "";
			pairs[2] = NULL;
			replace_text(&pruned, source->buffer, pairs);
			free(source->buffer);
			*source = pruned;
			memset(&pruned, 0, sizeof(pruned));
			pairs[0] = attribute;
			pairs[1] = kind ? "vec4(0.0, 0.0, 1.0, 1.0)" : "vec4(0.0, 0.0, 0.0, 1.0)";
			replace_words(&pruned, source->buffer, source->length, pairs);
			free(source->buffer);
			*source = pruned;
			for (index = 0; index < shader->attribute_count; index++)
			{
				if (!strcmp(shader->attribute_names[index], attribute))
				{
					shader->attribute_locations[index] = -1;
					break;
				}
			}
			break;
		}
	}
}

/* D3DVSDT_NORMPACKED3, unpacked with bit shifts: ES 1.00 has none and no
integer attributes, and the skinned shaders no room to do it in floats. The
attribute is the layer's copy of them as normalized shorts (normals_apply),
passed on as it is; a program that does not link so, or
HALO_GLES2_FLAT_NORMALS, has every such normal straight up
(normals_flatten) */
#define NORMPACKED3_UNPACK "vec4 unpack_normpacked3(vec4 p) { return p; }\n"
#define NORMPACKED3_FLAT "vec4 unpack_normpacked3(vec4 p) { return vec4(0.0, 0.0, 1.0, 1.0); }\n"

/* a skinned shader's node indices, as the renderer's model shaders take
them: "(v<n>.xyzw) * (c_<k>.wwww)" a little before its first
"a0 = int(floor(" (a node's index times three, the rows of its matrix) */
static void node_index_find(struct layer_shader *shader, const char *text)
{
	const char *a0 = strstr(text, "a0 = int(floor(");
	const char *weighted = strstr(text, ".xxxx) * (c_bones[");
	const char *at;

	/* (the first node's weight: "(v<n>.xxxx) * (c_bones[") */
	for (at = weighted; at && at > text && weighted - at < 8; at--)
	{
		int attribute;

		if (at[0] == '(' && at[1] == 'v' && sscanf(at, "(v%d.xxxx)", &attribute) == 1)
		{
			shader->weight_attribute = attribute;
			break;
		}
	}
	if (!a0)
		return;
	for (at = a0; at > text && a0 - at < 400; at--)
	{
		int attribute, scale;

		if (at[0] == '(' && at[1] == 'v' && sscanf(at, "(v%d.xyzw) * (c_%d.wwww)", &attribute, &scale) == 2)
		{
			shader->node_attribute = attribute;
			shader->node_scale = scale;
			return;
		}
	}
}

static char *shader_translate(struct layer_shader *shader, const char *source)
{
	static const char *const fragment_words[] = {
		"sampler3D", "sampler2D",
		"gl_FragDepth", "gl_FragDepthEXT",
		"uint", "float",
		NULL,
	};
	/* (the packed normals' attribute: no integers in ES 1.00's attributes,
	nor bit shifts to unpack them with) */
	static const char *const vertex_words[] = {
		"sampler3D", "sampler2D",
		"uint", "float",
		NULL,
	};
	struct text body = { 0 }, result = { 0 };
	const char *line = source;
	int fragment = shader->type == GL_FRAGMENT_SHADER;
	const char *const *words = fragment ? fragment_words : vertex_words;
	int skipping = 0;

	shader->attribute_count = 0;
	while (*line)
	{
		const char *end = strchr(line, '\n');
		size_t length = end ? (size_t)(end - line) : strlen(line);
		const char *text = line;
		char declaration[512];
		char name[64];

		while (*text == ' ' || *text == '\t')
			text++;
		if (skipping)
		{
			/* to the function's closing brace */
			if (*text == '}')
				skipping = 0;
		}
		else if (starts_with(text, "vec4 unpack_normpacked3("))
		{
			text_add(&body, normals_flat() ? NORMPACKED3_FLAT : NORMPACKED3_UNPACK);
			skipping = !memchr(text, '}', length - (size_t)(text - line));
		}
		else if (starts_with(text, "vec4 nv2a_finite("))
		{
			/* (0.9.1's: a not-a-number component made 0, by its bits, which
			ES 1.00 has no operations on, nor isnan: as it is, as before
			0.9.1. xgpu.h leaves a console its own body) */
			text_add(&body, "vec4 nv2a_finite(vec4 v) { return v; }\n");
			skipping = !memchr(text, '}', length - (size_t)(text - line));
		}
		else if (length >= sizeof(declaration))
		{
			replace_words(&body, line, length, words);
			text_add(&body, "\n");
		}
		else if (starts_with(text, "#version") || (starts_with(text, "precision") &&
			(strstr(text, "sampler3D") || strstr(text, "sampler2DArray") || strstr(text, "Shadow"))) ||
			strstr(text, "atomic_uint") || strstr(text, "early_fragment_tests"))
		{
			/* (ES 1.00 has none of them; the #version comes first, below) */
		}
		else
		{
			int location = -1;

			memcpy(declaration, text, length - (size_t)(text - line));
			declaration[length - (size_t)(text - line)] = 0;
			text = declaration;
			if (starts_with(text, "layout("))
			{
				const char *close = strchr(text, ')');
				const char *at = strstr(text, "location");

				if (at && close && at < close)
				{
					at = strchr(at, '=');
					if (at)
						location = atoi(at + 1);
				}
				text = close ? close + 1 : text;
				while (*text == ' ')
					text++;
			}
			while (starts_with(text, "flat ") || starts_with(text, "smooth ") || starts_with(text, "centroid ") ||
				starts_with(text, "noperspective ") || starts_with(text, "highp ") || starts_with(text, "mediump "))
			{
				text = strchr(text, ' ') + 1;
			}
			if (starts_with(text, "in ") && strchr(text, ';') && !strchr(text, '('))
			{
				text_add(&body, fragment ? "varying " : "attribute ");
				declaration_name(text + 3, name, sizeof(name));
				/* (a packed normal: its copy, four normalized shorts) */
				if (!fragment && strlen(name) > 7 && !strcmp(name + strlen(name) - 7, "_packed"))
				{
					text_add(&body, "vec4 ");
					text_add(&body, name);
					text_add(&body, ";");
				}
				else
					replace_words(&body, text + 3, strlen(text + 3), words);
				text_add(&body, "\n");
				if (!fragment && shader->attribute_count < LAYER_ATTRIBUTES && location >= 0)
				{
					declaration_name(text + 3, name, sizeof(name));
					strncpy(shader->attribute_names[shader->attribute_count], name,
						sizeof(shader->attribute_names[0]) - 1);
					shader->attribute_locations[shader->attribute_count++] = location;
				}
			}
			else if (starts_with(text, "out ") && strchr(text, ';') && !strchr(text, '('))
			{
				if (fragment)
				{
					declaration_name(text + 4, name, sizeof(name));
					text_add(&body, "#define ");
					text_add(&body, name);
					text_add(&body, " gl_FragColor\n");
				}
				else
				{
					text_add(&body, "varying ");
					replace_words(&body, text + 4, strlen(text + 4), words);
					text_add(&body, "\n");
				}
			}
			else
			{
				replace_words(&body, line, length, words);
				text_add(&body, "\n");
			}
		}
		line = end ? end + 1 : line + length;
	}

	text_add(&result, "#version 100\n");
	if (fragment)
	{
		if (body.buffer && (strstr(body.buffer, "dFdx") || strstr(body.buffer, "dFdy") || strstr(body.buffer, "fwidth")))
			text_add(&result, "#extension GL_OES_standard_derivatives : enable\n");
		if (body.buffer && strstr(body.buffer, "gl_FragDepthEXT"))
			text_add(&result, "#extension GL_EXT_frag_depth : enable\n");
	}
	text_add(&result, "precision highp float;\nprecision highp int;\n");
	text_add(&result, common_helpers);
	text_add(&result, fragment ? fragment_helpers : vertex_helpers);
	/* (only where one texture is sampled: the motion tracker's and the
	HUD's, not the level's and the models', whose many lookups would each
	pay for the test) */
	if (fragment && body.buffer && strstr(body.buffer, "texture(tex") &&
		!strstr(strstr(body.buffer, "texture(tex") + 1, "texture(tex"))
	{
		struct text bordered = { 0 };
		int unit;

		replace_text(&bordered, body.buffer, border_replacements);
		free(body.buffer);
		body = bordered;

		text_add(&result, border_helpers_head);
		for (unit = 0; unit < 4; unit++)
		{
			char helper[sizeof(border_helper_unit) + 64];

			snprintf(helper, sizeof(helper), border_helper_unit, unit, unit, unit, unit, unit, unit, unit, unit, unit);
			text_add(&result, helper);
		}
	}
	if (body.buffer)
		replace_text(&result, body.buffer, text_replacements);
	free(body.buffer);
	/* (HALO_GLES2_MEDIUMP: the pixel shaders at medium precision, to see
	what it saves the GPU) */
	if (fragment && result.buffer && getenv("HALO_GLES2_MEDIUMP"))
	{
		static const char *const medium[] = {
			"precision highp float;", "precision mediump float;",
			"precision highp int;", "precision mediump int;",
			NULL,
		};
		struct text lowered = { 0 };

		replace_text(&lowered, result.buffer, medium);
		free(result.buffer);
		result = lowered;
	}
	if (!fragment && result.buffer)
	{
		/* (HALO_GLES2_NO_FOLD: as written, to compare) */
		if (!getenv("HALO_GLES2_NO_FOLD"))
			operands_fold(&result);
		if (result.buffer)
		{
			int relative = strstr(result.buffer, "(a0 +") != NULL;

			if (relative)
				constants_keep(&result);
			constants_split(&result, relative);
		}
		attributes_prune(shader, &result);
	}
	shader->node_attribute = shader->node_scale = shader->weight_attribute = -1;
	if (!fragment && result.buffer)
		node_index_find(shader, result.buffer);
	return result.buffer;
}

static void specialized_forget(GLuint fragment);

static GLuint GL_APIENTRY layer_CreateShader(GLenum type)
{
	GLuint shader = real.CreateShader(type);

	if (shader && shader < LAYER_TRACKED)
	{
		/* (a name used again is another shader: no copy made of the last) */
		specialized_forget(shader);
		free(shaders[shader].source);
		memset(&shaders[shader], 0, sizeof(shaders[shader]));
		shaders[shader].type = type;
	}
	return shader;
}

static void GL_APIENTRY layer_ShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings,
	const GLint *lengths)
{
	struct text source = { 0 };
	char *translated;
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		if (lengths && lengths[index] >= 0)
			text_append(&source, strings[index], (size_t)lengths[index]);
		else
			text_add(&source, strings[index]);
	}
	if (!source.buffer || !shader || shader >= LAYER_TRACKED)
	{
		real.ShaderSource(shader, count, strings, lengths);
		free(source.buffer);
		return;
	}
	translated = shader_translate(&shaders[shader], source.buffer);
	free(source.buffer);
	free(shaders[shader].source);
	shaders[shader].source = translated;
	if (translated)
	{
		const GLchar *text = translated;

		real.ShaderSource(shader, 1, &text, NULL);
		/* (HALO_GLES2_DUMP_ALL: as given, deferred or not) */
		if (getenv("HALO_GLES2_DUMP_ALL"))
		{
			char path[512];
			FILE *file;

			snprintf(path, sizeof(path), "%s/source%u.%s", getenv("HALO_GLES2_DUMP_ALL"), shader,
				shaders[shader].type == GL_VERTEX_SHADER ? "vert" : "frag");
			file = fopen(path, "w");
			if (file)
			{
				fputs(translated, file);
				fclose(file);
			}
		}
	}
}

/* a shader that does not compile: its error and its translated source in
the log (the first few) */
static int program_binaries_on(void);

static void GL_APIENTRY layer_CompileShader(GLuint shader)
{
	static int reported;
	GLint compiled = 0;

	/* (with the programs' binaries kept, compiled at its program's link,
	if that finds no binary: layer_LinkProgram) */
	if (shader && shader < LAYER_TRACKED && shaders[shader].source && program_binaries_on())
	{
		shaders[shader].deferred = 1;
		counts.compiles++;
		return;
	}

	/* (HALO_GLES2_DUMP_ALL=folder: every shader as the driver gets it) */
	if (getenv("HALO_GLES2_DUMP_ALL") && shader < LAYER_TRACKED && shaders[shader].source)
	{
		char path[512];
		FILE *file;

		snprintf(path, sizeof(path), "%s/shader%u.%s", getenv("HALO_GLES2_DUMP_ALL"), shader,
			shaders[shader].type == GL_VERTEX_SHADER ? "vert" : "frag");
		file = fopen(path, "w");
		if (file)
		{
			fputs(shaders[shader].source, file);
			fclose(file);
		}
	}
	counts.compiles++;
	real.CompileShader(shader);
	halo_glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled && reported < 6 && shader < LAYER_TRACKED && shaders[shader].source)
	{
		char log[1024] = "";
		const char *line = shaders[shader].source;
		int number = 1;

		reported++;
		halo_glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		platform_log("gles2: a %s shader does not compile: %s", shaders[shader].type == GL_VERTEX_SHADER ? "vertex" :
			"fragment", log);
		while (*line)
		{
			const char *end = strchr(line, '\n');
			int length = end ? (int)(end - line) : (int)strlen(line);

			platform_log("gles2: %4d %.*s", number++, length, line);
			line += length + (end ? 1 : 0);
		}
	}
}

static void GL_APIENTRY layer_AttachShader(GLuint program, GLuint shader)
{
	if (program < LAYER_TRACKED && shader < LAYER_TRACKED && shaders[shader].type == GL_VERTEX_SHADER)
		program_vertex_shaders[program] = shader;
	else if (program < LAYER_TRACKED && shader < LAYER_TRACKED && shaders[shader].type == GL_FRAGMENT_SHADER)
		program_fragment_shaders[program] = shader;
	real.AttachShader(program, shader);
}

/* The pixel shaders divide each projected stage's texture coordinate by its
w (nv2a_psh.c: (vec4(xT0.xyz / (xT0.w != 0.0 ? xT0.w : 1.0), 1.0))), which
on the box is a third of a busy shader's time: 14 ms a full-screen pass, 10
without it. A vertex shader seldom writes a coordinate's w (4 of 69 in a10),
which then stays 1.0, and the divide does nothing. At a program's link the
layer looks at which its vertex shader writes, and the program gets a copy
of the pixel shader without the others' divides (kept for the next program
with the same pair of needs). */
struct specialized
{
	GLuint fragment;
	unsigned int mask;
	GLuint shader;
};

static struct specialized specialized[1024];
static int specialized_count;

static void specialized_forget(GLuint fragment)
{
	int index = 0;

	while (index < specialized_count)
	{
		if (specialized[index].fragment == fragment)
			specialized[index] = specialized[--specialized_count];
		else
			index++;
	}
}

/* the coordinates whose w the vertex shader writes (a bit each, oT0 to oT3) */
static unsigned int texture_w_written(const char *source)
{
	unsigned int written = 0;
	const char *line = source;

	while (line && *line)
	{
		const char *end = strchr(line, '\n');
		const char *text = line;
		size_t length = end ? (size_t)(end - line) : strlen(line);

		while (*text == ' ' || *text == '\t')
			text++;
		if (text[0] == 'o' && text[1] == 'T' && text[2] >= '0' && text[2] <= '3')
		{
			const char *equals = strstr(text, " = ");
			const char *swizzle = text + 3;

			/* "oT1.w = ", "oT2.xw = ", "oT3 = ": the w written */
			if (equals && equals < line + length &&
				(swizzle == equals || (*swizzle == '.' && memchr(swizzle, 'w', (size_t)(equals - swizzle)))))
			{
				written |= 1u << (text[2] - '0');
			}
		}
		line = end ? end + 1 : line + length;
	}
	return written;
}

static GLuint fragment_specialized(GLuint fragment, unsigned int unwritten)
{
	static void (GL_APIENTRY *get_shaderiv)(GLuint, GLenum, GLint *);
	struct text text = { 0 };
	const char *pairs[9];
	char divides[4][96], plain[4][8];
	int index, count = 0;
	GLuint shader;
	GLint compiled = 0;

	for (index = 0; index < specialized_count; index++)
	{
		if (specialized[index].fragment == fragment && specialized[index].mask == unwritten)
			return specialized[index].shader;
	}
	for (index = 0; index < 4; index++)
	{
		if (!(unwritten & (1u << index)))
			continue;
		snprintf(divides[index], sizeof(divides[index]), "(vec4(xT%d.xyz / (xT%d.w != 0.0 ? xT%d.w : 1.0), 1.0))", index,
			index, index);
		snprintf(plain[index], sizeof(plain[index]), "xT%d", index);
		pairs[count * 2] = divides[index];
		pairs[count * 2 + 1] = plain[index];
		count++;
	}
	pairs[count * 2] = NULL;
	/* (none to take out: as it is) */
	if (!count || (!strstr(shaders[fragment].source, "xT0.w != 0.0") && !strstr(shaders[fragment].source, "xT1.w != 0.0") &&
		!strstr(shaders[fragment].source, "xT2.w != 0.0") && !strstr(shaders[fragment].source, "xT3.w != 0.0")))
	{
		return 0;
	}
	replace_text(&text, shaders[fragment].source, pairs);
	if (!text.buffer || !strcmp(text.buffer, shaders[fragment].source))
	{
		free(text.buffer);
		return 0;
	}
	shader = real.CreateShader(GL_FRAGMENT_SHADER);
	if (!shader || shader >= LAYER_TRACKED)
	{
		free(text.buffer);
		return 0;
	}
	{
		const GLchar *source = text.buffer;

		real.ShaderSource(shader, 1, &source, NULL);
	}
	counts.compiles++;
	/* (with the programs' binaries kept, compiled only at a link that finds
	no binary, as the renderer's own: layer_LinkProgram goes back to the
	shader with its divides should it not do then. Compiled at each start,
	these were 19 of its 63 seconds) */
	if (!program_binaries_on())
	{
		real.CompileShader(shader);
		if (!get_shaderiv)
			get_shaderiv = (__typeof__(get_shaderiv))SDL_GL_GetProcAddress("glGetShaderiv");
		get_shaderiv(shader, GL_COMPILE_STATUS, &compiled);
		if (!compiled)
		{
			LAYER_ONCE("a pixel shader without its divides does not compile: the divides are kept");
			free(text.buffer);
			return 0;
		}
	}
	free(shaders[shader].source);
	shaders[shader].source = text.buffer;
	shaders[shader].type = GL_FRAGMENT_SHADER;
	shaders[shader].deferred = program_binaries_on();
	if (specialized_count < (int)(sizeof(specialized) / sizeof(*specialized)))
	{
		specialized[specialized_count].fragment = fragment;
		specialized[specialized_count].mask = unwritten;
		specialized[specialized_count].shader = shader;
		specialized_count++;
	}
	return shader;
}

/* a program's two shaders, as given the driver, in HALO_GLES2_DUMP */
static void program_dump(GLuint program, const char *tag)
{
	const char *folder = getenv("HALO_GLES2_DUMP");
	GLuint pair[2];
	int stage;

	if (!folder || program >= LAYER_TRACKED)
		return;
	pair[0] = program_vertex_shaders[program];
	pair[1] = program_fragment_shaders[program];
	for (stage = 0; stage < 2; stage++)
	{
		char path[512];
		FILE *file;

		if (!pair[stage] || !shaders[pair[stage]].source)
			continue;
		snprintf(path, sizeof(path), "%s/%s%u.%s", folder, tag, program, stage ? "frag" : "vert");
		file = fopen(path, "w");
		if (file)
		{
			fputs(shaders[pair[stage]].source, file);
			fclose(file);
		}
	}
}

/* (each program's c[], layer_GetUniformLocation's) */
struct constant_split;
static struct constant_split *constant_split[LAYER_TRACKED];

/* ---------- program binaries

The renderer keeps its shaders' texts (xgpu_shader_cache.c) and at every
start compiles them all and links every pair, made for desktop drivers that
compile in parallel: on the box's one core and Vivante's compiler, 209
shaders and 160 programs took a minute at each start, and longer as more
maps are played (26 seconds with their binaries: 11 the driver's loads, 6
the texts' translation). The driver has GL_OES_get_program_binary: each program
linked from its texts is kept as the driver's binary, in gles2_programs in
the save folder, under a hash of its two texts and its attributes' places;
the next link of the same finds it and loads it, its shaders never compiled
(layer_CompileShader leaves a shader for its link). A binary the driver no
longer takes (a new driver) is linked from its texts again, and kept anew.
HALO_GLES2_NO_BINARIES: none kept, every shader compiled as asked. */

#define PROGRAM_BINARY_VERSION "gles2 program 1"

static struct
{
	int state;
	char folder[1024];
	void (GL_APIENTRY *get)(GLuint, GLsizei, GLsizei *, GLenum *, void *);
	void (GL_APIENTRY *load)(GLuint, GLenum, const void *, GLint);
	unsigned long loaded, saved, linked;
} program_binaries = { -1 };

static int program_binaries_on(void)
{
	if (program_binaries.state < 0)
	{
		const char *root = getenv("HALO_SAVE_ROOT");

		program_binaries.state = 0;
		if (!root || !*root || getenv("HALO_GLES2_NO_BINARIES") || !host_gl_has_extension("GL_OES_get_program_binary"))
			return 0;
		program_binaries.get = (__typeof__(program_binaries.get))SDL_GL_GetProcAddress("glGetProgramBinaryOES");
		program_binaries.load = (__typeof__(program_binaries.load))SDL_GL_GetProcAddress("glProgramBinaryOES");
		snprintf(program_binaries.folder, sizeof(program_binaries.folder), "%s/gles2_programs", root);
		SDL_CreateDirectory(program_binaries.folder);
		program_binaries.state = program_binaries.get && program_binaries.load;
		platform_log("gles2: programs %s", program_binaries.state ? "kept as the driver's binaries" :
			"not kept (no binary calls)");
	}
	return program_binaries.state;
}

static unsigned long long program_hash_add(unsigned long long hash, const void *data, size_t length)
{
	const unsigned char *bytes = data;
	size_t index;

	for (index = 0; index < length; index++)
		hash = (hash ^ bytes[index]) * 0x100000001b3ULL;
	return hash;
}

/* the hash of a program's two texts and its attributes' places */
static unsigned long long program_key(GLuint vertex, GLuint fragment)
{
	unsigned long long hash = 0xcbf29ce484222325ULL;
	int index;

	hash = program_hash_add(hash, PROGRAM_BINARY_VERSION, sizeof(PROGRAM_BINARY_VERSION));
	hash = program_hash_add(hash, shaders[vertex].source, strlen(shaders[vertex].source) + 1);
	hash = program_hash_add(hash, shaders[fragment].source, strlen(shaders[fragment].source) + 1);
	for (index = 0; index < shaders[vertex].attribute_count; index++)
	{
		hash = program_hash_add(hash, shaders[vertex].attribute_names[index],
			strlen(shaders[vertex].attribute_names[index]) + 1);
		hash = program_hash_add(hash, &shaders[vertex].attribute_locations[index],
			sizeof(shaders[vertex].attribute_locations[index]));
	}
	return hash;
}

static void program_compile_now(GLuint shader)
{
	GLint compiled = 0;

	if (!shader || shader >= LAYER_TRACKED || !shaders[shader].deferred)
		return;
	shaders[shader].deferred = 0;
	real.CompileShader(shader);
	halo_glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled)
	{
		char log[512] = "";

		halo_glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		platform_log("gles2: a %s shader does not compile: %s", shaders[shader].type == GL_VERTEX_SHADER ? "vertex" :
			"fragment", log);
	}
}

/* a program linked: from its binary when there is one, else from its texts
(and its binary kept). Whether it is linked */
static GLint program_link(GLuint program, GLuint vertex, GLuint fragment)
{
	char path[1100];
	GLint linked = 0;
	int keyed = program_binaries_on() && vertex && vertex < LAYER_TRACKED && fragment && fragment < LAYER_TRACKED &&
		shaders[vertex].source && shaders[fragment].source;

	if (keyed)
	{
		FILE *file;

		snprintf(path, sizeof(path), "%s/%016llx.bin", program_binaries.folder, program_key(vertex, fragment));
		file = fopen(path, "rb");
		if (file)
		{
			unsigned char *data = NULL;
			unsigned int format = 0;
			long length = 0;

			if (fread(&format, sizeof(format), 1, file) == 1 && !fseek(file, 0, SEEK_END) &&
				(length = ftell(file) - (long)sizeof(format)) > 0 && (data = malloc((size_t)length)) != NULL &&
				!fseek(file, (long)sizeof(format), SEEK_SET) && fread(data, 1, (size_t)length, file) == (size_t)length)
			{
				program_binaries.load(program, (GLenum)format, data, (GLint)length);
				halo_glGetProgramiv(program, GL_LINK_STATUS, &linked);
			}
			free(data);
			fclose(file);
			if (linked)
			{
				program_binaries.loaded++;
				return linked;
			}
		}
	}
	program_compile_now(vertex);
	program_compile_now(fragment);
	real.LinkProgram(program);
	halo_glGetProgramiv(program, GL_LINK_STATUS, &linked);
	program_binaries.linked++;
	if (keyed && linked)
	{
		GLint length = 0;

		halo_glGetProgramiv(program, 0x8741 /* GL_PROGRAM_BINARY_LENGTH_OES */, &length);
		if (length > 0)
		{
			unsigned char *data = malloc((size_t)length);
			GLsizei written = 0;
			GLenum format = 0;

			if (data)
			{
				FILE *file;

				program_binaries.get(program, length, &written, &format, data);
				file = written > 0 ? fopen(path, "wb") : NULL;
				if (file)
				{
					unsigned int format_word = (unsigned int)format;

					fwrite(&format_word, sizeof(format_word), 1, file);
					fwrite(data, 1, (size_t)written, file);
					fclose(file);
					program_binaries.saved++;
				}
				free(data);
			}
		}
	}
	return linked;
}

/* a deferred shader is compiled as far as the renderer asks */
static void (GL_APIENTRY *real_GetShaderiv)(GLuint, GLenum, GLint *);

static void GL_APIENTRY layer_GetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
	if (pname == GL_COMPILE_STATUS && shader && shader < LAYER_TRACKED && shaders[shader].deferred)
	{
		*params = GL_TRUE;
		return;
	}
	real_GetShaderiv(shader, pname, params);
}

/* Lazy programs. The renderer's shader cache links every program it has ever
drawn at each start (xgpu_shader_cache_warm) and asks each for its uniforms:
on a box that has been played, hundreds of programs, half a minute and tens
of megabytes for programs most sessions never draw. A program whose binary is
on the stick is therefore linked in name only: its link status is true, its
uniforms get locations of the layer's own (LAZY_LOCATION + a slot), its
samplers' units are kept, and the binary is loaded at its first draw or
first other uniform (program_ready). HALO_GLES2_EAGER links all at once, as
before. */
#define LAZY_LOCATION 0x200000
#define LAZY_SLOTS 96

struct lazy_program
{
	int pending, wants_constants;
	GLuint fragment;
	int count;
	/* each slot: its name (lazy_names'), the driver's location once
	loaded, and a glUniform1i kept for then */
	unsigned char names[LAZY_SLOTS];
	unsigned char has_value[LAZY_SLOTS];
	GLint locations[LAZY_SLOTS];
	GLint values[LAZY_SLOTS];
};

static struct lazy_program *lazy_programs[LAYER_TRACKED];
static char *lazy_names[256];
static int lazy_name_count;
static struct
{
	unsigned long deferred, readied;
} lazy_counts;
static void (GL_APIENTRY *real_GetProgramiv)(GLuint, GLenum, GLint *);
static void (GL_APIENTRY *real_Uniform1i)(GLint, GLint);
static void (GL_APIENTRY *real_Uniform1f)(GLint, GLfloat);
static void (GL_APIENTRY *real_Uniform2f)(GLint, GLfloat, GLfloat);
static void program_link_finish(GLuint program, GLuint attached_fragment);
static int constants_split_build(GLuint program);
static __typeof__(halo_glGetUniformLocation) real_GetUniformLocation;

static int lazy_on(void)
{
	static int on = -1;

	if (on < 0)
		on = program_binaries_on() && !getenv("HALO_GLES2_EAGER");
	return on;
}

static struct lazy_program *lazy_pending(GLuint program)
{
	return program < LAYER_TRACKED && lazy_programs[program] && lazy_programs[program]->pending ?
		lazy_programs[program] : NULL;
}

static int program_is_pending(GLuint program)
{
	return lazy_pending(program) != NULL;
}

/* a name's number among the lazy programs' uniform names, -1 for no room */
static int lazy_name(const char *name)
{
	int index;

	for (index = 0; index < lazy_name_count; index++)
	{
		if (!strcmp(lazy_names[index], name))
			return index;
	}
	if (lazy_name_count == 256 || !(lazy_names[lazy_name_count] = strdup(name)))
		return -1;
	return lazy_name_count++;
}

/* a lazy program loaded now, with what was asked of it meanwhile */
static void program_ready(GLuint program)
{
	struct lazy_program *lazy = lazy_pending(program);
	int slot;

	if (!lazy)
		return;
	lazy->pending = 0;
	if (lazy_counts.readied++ % 50 == 0)
		platform_log("gles2: lazy programs: %lu loaded of %lu left for their first draw", lazy_counts.readied,
			lazy_counts.deferred);
	program_link_finish(program, lazy->fragment);
	if (lazy->wants_constants)
		constants_split_build(program);
	real.UseProgram(program);
	for (slot = 0; slot < lazy->count; slot++)
	{
		lazy->locations[slot] = real_GetUniformLocation(program, lazy_names[lazy->names[slot]]);
		if (lazy->has_value[slot] && lazy->locations[slot] >= 0)
			real_Uniform1i(lazy->locations[slot], lazy->values[slot]);
	}
	if (program_current != program)
		real.UseProgram(lazy_pending(program_current) ? 0 : program_current);
}

/* a uniform's location as the driver has it: a lazy program's slot is its
program's own (the current program's, loaded if it was not) */
static GLint lazy_location(GLint location)
{
	struct lazy_program *lazy;

	/* (the driver's own: of a program that is loaded, the layer's blit too) */
	if (location < LAZY_LOCATION || location >= LAZY_LOCATION + LAZY_SLOTS)
		return location;
	lazy = program_current < LAYER_TRACKED ? lazy_programs[program_current] : NULL;
	if (!lazy || location - LAZY_LOCATION >= lazy->count)
		return -1;
	program_ready(program_current);
	return lazy->locations[location - LAZY_LOCATION];
}

static void GL_APIENTRY layer_GetProgramiv(GLuint program, GLenum pname, GLint *params)
{
	if (pname == GL_LINK_STATUS && lazy_pending(program))
	{
		*params = GL_TRUE;
		return;
	}
	program_ready(program);
	real_GetProgramiv(program, pname, params);
}

static GLint twin_uniform(GLint location, int kind, const GLfloat *values, GLint integer);
static void twin_forget(GLuint program);

static void GL_APIENTRY layer_Uniform1i(GLint location, GLint value)
{
	struct lazy_program *lazy;

	location = twin_uniform(location, 1, NULL, value);
	lazy = lazy_pending(program_current);

	/* (a sampler's unit, given when the program is adopted: kept) */
	if (lazy && location >= LAZY_LOCATION && location < LAZY_LOCATION + lazy->count)
	{
		lazy->has_value[location - LAZY_LOCATION] = 1;
		lazy->values[location - LAZY_LOCATION] = value;
		return;
	}
	location = lazy_location(location);
	if (location >= 0)
		real_Uniform1i(location, value);
}

static void GL_APIENTRY layer_Uniform1f(GLint location, GLfloat x)
{
	GLfloat values[4] = { x, 0.0f, 0.0f, 0.0f };

	location = lazy_location(twin_uniform(location, 2, values, 0));
	if (location >= 0)
		real_Uniform1f(location, x);
}

static void GL_APIENTRY layer_Uniform2f(GLint location, GLfloat x, GLfloat y)
{
	GLfloat values[4] = { x, y, 0.0f, 0.0f };

	location = lazy_location(twin_uniform(location, 3, values, 0));
	if (location >= 0)
		real_Uniform2f(location, x, y);
}

static void GL_APIENTRY layer_LinkProgram(GLuint program)
{
	GLuint attached_fragment = 0;

	/* (linked again: another program, its flat twin made anew if needed) */
	if (program < LAYER_TRACKED && twins[program])
	{
		twin_forget(program);
		if (program_renderer == program)
			program_current = program;
	}

	if (program < LAYER_TRACKED && lazy_programs[program])
	{
		free(lazy_programs[program]);
		lazy_programs[program] = NULL;
	}
	if (program < LAYER_TRACKED && position_programs[program])
	{
		free(position_programs[program]);
		position_programs[program] = NULL;
	}

	if (program < LAYER_TRACKED && program_vertex_shaders[program])
	{
		struct layer_shader *vertex = &shaders[program_vertex_shaders[program]];
		int index;

		for (index = 0; index < vertex->attribute_count; index++)
		{
			if (vertex->attribute_locations[index] < 0)
				continue;
			if (vertex->attribute_locations[index] < attribute_maximum)
				halo_glBindAttribLocation(program, (GLuint)vertex->attribute_locations[index],
					vertex->attribute_names[index]);
			else
				LAYER_ONCE("attribute %s at %d, past the box's %d", vertex->attribute_names[index],
					vertex->attribute_locations[index], attribute_maximum);
		}
	}
	/* (its pixel shader without the divides its vertex shader makes no use
	for, fragment_specialized) */
	if (program < LAYER_TRACKED && program_vertex_shaders[program] && program_fragment_shaders[program] &&
		shaders[program_vertex_shaders[program]].source && shaders[program_fragment_shaders[program]].source &&
		!getenv("HALO_GLES2_KEEP_DIVIDES"))
	{
		static void (GL_APIENTRY *detach)(GLuint, GLuint);
		unsigned int unwritten = ~texture_w_written(shaders[program_vertex_shaders[program]].source) & 15u;
		GLuint copy = unwritten ? fragment_specialized(program_fragment_shaders[program], unwritten) : 0;

		attached_fragment = copy ? copy : program_fragment_shaders[program];

		if (!detach)
			detach = (__typeof__(detach))SDL_GL_GetProcAddress("glDetachShader");
		if (copy && detach)
		{
			detach(program, program_fragment_shaders[program]);
			real.AttachShader(program, copy);
		}
	}
	/* (its binary is on the stick: linked at its first use, program_ready) */
	if (lazy_on() && attached_fragment && program_vertex_shaders[program])
	{
		char path[1100];
		FILE *file;

		snprintf(path, sizeof(path), "%s/%016llx.bin", program_binaries.folder,
			program_key(program_vertex_shaders[program], attached_fragment));
		file = fopen(path, "rb");
		if (file)
		{
			struct lazy_program *lazy = calloc(1, sizeof(*lazy));

			fclose(file);
			if (lazy)
			{
				lazy->pending = 1;
				lazy->fragment = attached_fragment;
				lazy_programs[program] = lazy;
				lazy_counts.deferred++;
				free(constant_split[program]);
				constant_split[program] = NULL;
				memset(&border_programs[program], 0, sizeof(border_programs[program]));
				return;
			}
		}
	}
	program_link_finish(program, attached_fragment);
}

/* the start of the line text's at is on */
static const char *line_of(const char *text, const char *at)
{
	while (at > text && at[-1] != '\n')
		at--;
	return at;
}

/* the name a line assigns to as a whole ("\tvec4 v1r0_sum = ..."; none for
"\tr0.a = ..."), in name: whether it assigns one */
static int line_target(const char *line, char *name, size_t size)
{
	static const char *const types[] = { "vec4 ", "vec3 ", "vec2 ", "float " };
	size_t index, take = 0;

	while (*line == '\t' || *line == ' ')
		line++;
	for (index = 0; index < sizeof(types) / sizeof(*types); index++)
	{
		if (!strncmp(line, types[index], strlen(types[index])))
		{
			line += strlen(types[index]);
			break;
		}
	}
	while (is_name_character(line[take]) && take + 1 < size)
		take++;
	memcpy(name, line, take);
	name[take] = 0;
	return take && !strncmp(line + take, " = ", 3);
}

/* whether a pixel shader reads name: not where a line assigns to it (nor
its reads of itself there, "x = clamp(x, 0.0, 1.0)"), nor on a line that
assigns a value read nowhere itself (nv2a_psh.c's v1r0_sum where the final
combiner takes no v1 + r0). Anything it cannot tell, a read */
static int name_read(const char *text, const char *name, int depth)
{
	size_t length = strlen(name);
	const char *at = text;

	while ((at = strstr(at, name)) != NULL)
	{
		const char *line = line_of(text, at);
		char target[48];

		if ((at != text && is_name_character(at[-1])) || is_name_character(at[length]) ||
			!strncmp(line, "varying ", 8))
		{
			at += length;
			continue;
		}
		if (!line_target(line, target, sizeof(target)) || !strncmp(target, "gl_", 3) ||
			!strcmp(target, "fragment_color") ||
			(strcmp(target, name) && (depth >= 2 || name_read(text, target, depth + 1))))
		{
			return 1;
		}
		at += length;
	}
	return 0;
}

/* whether a pixel shader reads a vertex output (xD1), itself or through
the copy it starts with ("\tvec4 v1 = xD1;") */
static int output_read(const char *fragment, const char *output)
{
	const char *at = fragment;
	size_t length = strlen(output);

	while ((at = strstr(at, output)) != NULL)
	{
		const char *line = line_of(fragment, at);
		char alias[16];

		if ((at != fragment && is_name_character(at[-1])) || is_name_character(at[length]) ||
			!strncmp(line, "varying ", 8))
		{
			at += length;
			continue;
		}
		if (sscanf(line, " vec4 %15[A-Za-z0-9_] = ", alias) != 1 || !strcmp(alias, output) || at[length] != ';' ||
			name_read(fragment, alias, 0))
		{
			return 1;
		}
		at += length;
	}
	return 0;
}

/* A skinned shader's node reads clamped once where a0 is set (to the first
row of c_bones' last node), not at each read: c_bones[a0 + 1] for
c_bones[clamp_int(a0 + 1, 0, 107)], as every read is of one of a node's
three rows from a0. Only a row past the last node reads another, the filler
of a vertex's unweighted second node. Whether it was done (not where a read
is past a0 + 2) */
static int bones_reads_clamped_once(struct text *source)
{
	static const char *const read = "c_bones[clamp_int(a0 + ";
	static const char *const set = "a0 = int(floor(";
	struct text result = { 0 };
	const char *at;
	char clamp_end[32], rows_last[24];

	snprintf(clamp_end, sizeof(clamp_end), ", 0, %d)]", BONES_COUNT - 1);
	snprintf(rows_last, sizeof(rows_last), "), 0, %d);", BONES_COUNT - 3);
	for (at = source->buffer; at && (at = strstr(at, read)) != NULL; at += strlen(read))
	{
		char *end;
		long row = strtol(at + strlen(read), &end, 10);

		if (row < 0 || row > 2 || strncmp(end, clamp_end, strlen(clamp_end)))
			return 0;
	}
	for (at = source->buffer; at && *at;)
	{
		const char *next_read = strstr(at, read), *next_set = strstr(at, set);

		if (next_read && (!next_set || next_read < next_set))
		{
			char *end;
			long row = strtol(next_read + strlen(read), &end, 10);
			char text[32];

			text_append(&result, at, (size_t)(next_read - at));
			snprintf(text, sizeof(text), "c_bones[a0 + %ld]", row);
			text_add(&result, text);
			at = end + strlen(clamp_end);
		}
		else if (next_set)
		{
			/* ("a0 = int(floor(m1.x + 0.001));" as
			"a0 = clamp_int(int(floor(m1.x + 0.001)), 0, 105);") */
			const char *close = strstr(next_set, ");\n");

			if (!close)
			{
				free(result.buffer);
				return 0;
			}
			text_append(&result, at, (size_t)(next_set - at));
			text_add(&result, "a0 = clamp_int(");
			text_append(&result, next_set + 5, (size_t)(close - (next_set + 5)));
			text_add(&result, rows_last);
			at = close + 2;
		}
		else
		{
			text_add(&result, at);
			break;
		}
	}
	if (!result.buffer)
		return 0;
	free(source->buffer);
	*source = result;
	return 1;
}

/* A vertex shader too large for the box's compiler (with the game's
VC_ENABLE_LTC=0: what depends only on uniforms is worked out at each
vertex) links not and is not drawn: the skinned shaders of the
first-person weapon and arms with their texture transforms, lights and
fog (on damnation the assault rifle's own, which the box drew none of; on
bloodgulch one such). Its program is linked again with a copy of the
shader that writes none of the outputs its pixel shader does not read (xD1
where it copies it to v1 and takes it nowhere, xB0, xB1, xT2 and xT3 where
it has no such stage: the compiler leaves out what computes them), whose
position, the clip space one it kept (clip_position), is scaled and offset
by two uniforms the layer works out (position_uniforms_apply) in place of
the screen-space conversion undone with c[58], c[59], the viewport and the
half-pixel offset, and whose node reads are clamped once
(bones_reads_clamped_once). Each of those uniforms is still named, where
it is never taken, so the copy keeps them for the renderer to give.
Whether it is linked; the copy stays attached */
static GLint vertex_pruned_link(GLuint program)
{
	static const char *const outputs[] = { "xD0", "xD1", "xB0", "xB1", "xT0", "xT1", "xT2", "xT3", "xFog" };
	static const char *const conversion_start = "\tvec3 scale = vec3(viewport_scale.x != 0.0";
	static const char *const conversion_end = "\tif (!(abs(position.w) > 0.0))";
	static const char *const bones_kept = "\tif (point_size < -1.0e30) gl_Position = c_bones[107];\n";
	static void (GL_APIENTRY *detach)(GLuint, GLuint);
	static void (GL_APIENTRY *delete_shader)(GLuint);
	GLuint vertex = program_vertex_shaders[program], fragment = program_fragment_shaders[program], copy;
	struct text pruned = { 0 };
	const char *at, *end;
	const GLchar *source;
	size_t index;
	int removed = 0, converted = 0;
	GLint status = 0;
	char names[64] = "";

	if (!vertex || vertex >= LAYER_TRACKED || !fragment || fragment >= LAYER_TRACKED || !shaders[vertex].source ||
		!shaders[fragment].source)
	{
		return 0;
	}
	if (!detach)
		detach = (__typeof__(detach))SDL_GL_GetProcAddress("glDetachShader");
	if (!delete_shader)
		delete_shader = (__typeof__(delete_shader))SDL_GL_GetProcAddress("glDeleteShader");
	if (!detach || !delete_shader)
		return 0;
	text_add(&pruned, shaders[vertex].source);
	for (index = 0; pruned.buffer && index < sizeof(outputs) / sizeof(*outputs); index++)
	{
		char line[16];
		struct text cut = { 0 };

		if (output_read(shaders[fragment].source, outputs[index]))
			continue;
		snprintf(line, sizeof(line), "\n\t%s = ", outputs[index]);
		at = strstr(pruned.buffer, line);
		end = at ? strchr(at + 1, '\n') : NULL;
		if (!end)
			continue;
		text_append(&cut, pruned.buffer, (size_t)(at - pruned.buffer));
		text_add(&cut, end);
		free(pruned.buffer);
		pruned = cut;
		removed++;
		strcat(names, " ");
		strcat(names, outputs[index]);
	}
	/* (the conversion: only where the clip space position is always kept,
	as the screen-space shaders keep it, straight through) */
	at = pruned.buffer ? strstr(pruned.buffer, conversion_start) : NULL;
	end = at ? strstr(at, conversion_end) : NULL;
	if (end && strstr(pruned.buffer, "\tclip_captured = true;\n") && strstr(pruned.buffer, "uniform vec4 c_58;\n") &&
		strstr(pruned.buffer, "uniform vec4 c_59;\n") && strstr(pruned.buffer, "uniform float screen_offset;\n") &&
		strstr(pruned.buffer, bones_kept))
	{
		static const char *const pairs[] = {
			"uniform float screen_offset;\n",
			"uniform float screen_offset;\nuniform vec4 layer_position_scale;\nuniform vec4 layer_position_offset;\n",
			"\tif (point_size < -1.0e30) gl_Position = c_bones[107];\n",
			"\tif (point_size < -1.0e30) gl_Position = c_bones[107] + viewport_scale + viewport_offset + c_58 + c_59 + "
				"vec4(screen_offset);\n",
			NULL,
		};
		struct text cut = { 0 }, declared = { 0 };

		text_append(&cut, pruned.buffer, (size_t)(at - pruned.buffer));
		text_add(&cut, "\tvec4 position = vec4(clip_position.xyz * layer_position_scale.xyz + clip_position.w * "
			"layer_position_offset.xyz, clip_position.w);\n");
		text_add(&cut, end);
		replace_text(&declared, cut.buffer, pairs);
		free(cut.buffer);
		free(pruned.buffer);
		pruned = declared;
		converted = 1;
	}
	if (converted)
		bones_reads_clamped_once(&pruned);
	if ((!removed && !converted) || !pruned.buffer)
	{
		free(pruned.buffer);
		return 0;
	}
	copy = real.CreateShader(GL_VERTEX_SHADER);
	if (!copy)
	{
		free(pruned.buffer);
		return 0;
	}
	source = pruned.buffer;
	real.ShaderSource(copy, 1, &source, NULL);
	real.CompileShader(copy);
	counts.compiles++;
	/* (HALO_GLES2_DUMP: the copy, in that folder) */
	if (getenv("HALO_GLES2_DUMP"))
	{
		char path[512];
		FILE *file;

		snprintf(path, sizeof(path), "%s/pruned%u.vert", getenv("HALO_GLES2_DUMP"), program);
		file = fopen(path, "w");
		if (file)
		{
			fputs(pruned.buffer, file);
			fclose(file);
		}
	}
	free(pruned.buffer);
	halo_glGetShaderiv(copy, GL_COMPILE_STATUS, &status);
	if (status)
	{
		detach(program, vertex);
		real.AttachShader(program, copy);
		real.LinkProgram(program);
		counts.links++;
		halo_glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (!status)
		{
			detach(program, copy);
			real.AttachShader(program, vertex);
		}
	}
	/* (deleted with the program it is attached to) */
	delete_shader(copy);
	platform_log("gles2: a program too large to link, again without the outputs its pixel shader does not read "
		"(%s)%s: %s", removed ? names + 1 : "none", converted ? " and with its position's conversion worked out" : "",
		status ? "linked" : "still does not link");
	if (status && converted)
	{
		position_programs[program] = calloc(1, sizeof(struct position_program));
		if (position_programs[program])
			position_programs[program]->scale = -2;
	}
	return status;
}

/* A vertex shader whose program does not link even pruned, with its packed
normals read: compiled again with them straight up (NORMPACKED3_FLAT), as
the layer had them all before. Whether it was */
static int normals_flatten(GLuint vertex)
{
	static const char *const pairs[] = { NORMPACKED3_UNPACK, NORMPACKED3_FLAT, NULL };
	struct text flat = { 0 };
	const GLchar *text;

	if (!vertex || vertex >= LAYER_TRACKED || !shaders[vertex].source ||
		!strstr(shaders[vertex].source, NORMPACKED3_UNPACK))
	{
		return 0;
	}
	replace_text(&flat, shaders[vertex].source, pairs);
	if (!flat.buffer)
		return 0;
	free(shaders[vertex].source);
	shaders[vertex].source = flat.buffer;
	text = flat.buffer;
	real.ShaderSource(vertex, 1, &text, NULL);
	/* (compiled by program_link, as a deferred one) */
	shaders[vertex].deferred = 1;
	return 1;
}

/* the link itself, and what the layer keeps of a linked program */
static void program_link_finish(GLuint program, GLuint attached_fragment)
{
	static int reported;
	GLint linked = 0;

	counts.links++;
	linked = program_link(program, program < LAYER_TRACKED ? program_vertex_shaders[program] : 0,
		attached_fragment ? attached_fragment : (program < LAYER_TRACKED ? program_fragment_shaders[program] : 0));
	/* (its pixel shader without its divides did not do: with them) */
	if (!linked && attached_fragment && program < LAYER_TRACKED && attached_fragment != program_fragment_shaders[program])
	{
		static void (GL_APIENTRY *detach_copy)(GLuint, GLuint);

		if (!detach_copy)
			detach_copy = (__typeof__(detach_copy))SDL_GL_GetProcAddress("glDetachShader");
		if (detach_copy)
		{
			LAYER_ONCE("a program without its pixel shader's divides does not link: the divides are kept");
			detach_copy(program, attached_fragment);
			real.AttachShader(program, program_fragment_shaders[program]);
			linked = program_link(program, program_vertex_shaders[program], program_fragment_shaders[program]);
		}
	}
	/* (too large for the box's compiler: vertex_pruned_link) */
	if (program < LAYER_TRACKED)
	{
		free(position_programs[program]);
		position_programs[program] = NULL;
		if (!linked)
			linked = vertex_pruned_link(program);
		if (!linked && normals_flatten(program_vertex_shaders[program]))
		{
			linked = program_link(program, program_vertex_shaders[program],
				attached_fragment ? attached_fragment : program_fragment_shaders[program]);
			if (!linked)
				linked = vertex_pruned_link(program);
			platform_log("gles2: a program that does not link with its packed normals, with them straight up: %s",
				linked ? "linked" : "still does not link");
		}
	}
	/* (a name used again is another program: its c[] is looked up anew) */
	if (program < LAYER_TRACKED)
	{
		program_attributes[program] = 0;
		free(constant_split[program]);
		constant_split[program] = NULL;
		border_programs[program].color = linked ? halo_glGetUniformLocation(program, "layer_border_color") + 1 : 0;
		border_programs[program].axes = linked ? halo_glGetUniformLocation(program, "layer_border_axes") + 1 : 0;
		border_programs[program].uploaded = 0;
	}
	if (!linked && reported < 6)
	{
		char log[1024] = "";

		reported++;
		halo_glGetProgramInfoLog(program, sizeof(log), NULL, log);
		/* (HALO_GLES2_DUMP: the pair that does not link, in that folder) */
		if (getenv("HALO_GLES2_DUMP") && program < LAYER_TRACKED)
		{
			GLuint pair[2] = { program_vertex_shaders[program], program_fragment_shaders[program] };
			int stage;

			for (stage = 0; stage < 2; stage++)
			{
				char path[512];
				FILE *file;

				if (!pair[stage] || !shaders[pair[stage]].source)
					continue;
				snprintf(path, sizeof(path), "%s/link%d.%s", getenv("HALO_GLES2_DUMP"), reported,
					stage ? "frag" : "vert");
				file = fopen(path, "w");
				if (file)
				{
					fputs(shaders[pair[stage]].source, file);
					fclose(file);
				}
			}
		}
		platform_log("gles2: a program does not link: %s", log);
	}
}

/* (the driver's own, which layer_GetUniformLocation and layer_Uniform4fv
stand in front of for c[]) */
static __typeof__(halo_glUniform4fv) real_Uniform4fv;

/* the current program's border uniforms, from the samplers on units 0 to 3,
given only when they change (nearly never: the motion tracker's draws) */
static void border_uniforms_apply(void)
{
	GLfloat values[2][4][4];
	GLuint unit;

	if (program_current >= LAYER_TRACKED || border_programs[program_current].color <= 0 ||
		border_programs[program_current].axes <= 0 || !real_Uniform4fv)
	{
		return;
	}
	memset(values, 0, sizeof(values));
	for (unit = 0; unit < 4; unit++)
	{
		const struct layer_sampler *sampler = unit_samplers[unit] && unit_samplers[unit] < LAYER_TRACKED ?
			&samplers[unit_samplers[unit]] : NULL;

		if (!sampler || (!sampler->border_s && !sampler->border_t))
			continue;
		memcpy(values[0][unit], sampler->border_color, sizeof(values[0][unit]));
		values[1][unit][0] = sampler->border_s ? 1.0f : 0.0f;
		values[1][unit][1] = sampler->border_t ? 1.0f : 0.0f;
	}
	if (border_programs[program_current].uploaded &&
		!memcmp(values, border_programs[program_current].values, sizeof(values)))
	{
		return;
	}
	real_Uniform4fv(border_programs[program_current].color - 1, 4, &values[0][0][0]);
	real_Uniform4fv(border_programs[program_current].axes - 1, 4, &values[1][0][0]);
	memcpy(border_programs[program_current].values, values, sizeof(values));
	border_programs[program_current].uploaded = 1;
}

/* c[]'s location as the renderer is given it: one no uniform has, so that
its uploads to c + k (d3d8_gl.c, k to 191) reach layer_Uniform4fv, which
sends each to where constants_split put that constant */
#define CONSTANTS_VIRTUAL_LOCATION 0x100000

/* a program's c[]: where each constant it names went (-1 none), and its
nodes' block (c_bones, c[60] on; -1 none) */
struct constant_split
{
	GLint named[192];
	GLint bones;
	/* every c[] the renderer gave, kept for bones_window_apply: whether any
	went past c_bones' last (c[167]), the node constant c_bones[0] holds now
	(as an offset from c[60]), and whether c_bones is behind the copy */
	GLfloat kept[192][4];
	int past_bones, window, window_stale;
};

static GLint uniform_location_find(GLuint program, const GLchar *name);
static void twin_name(GLuint program, const GLchar *name, GLint location);

static GLint GL_APIENTRY layer_GetUniformLocation(GLuint program, const GLchar *name)
{
	GLint location = uniform_location_find(program, name);

	twin_name(program, name, location);
	return location;
}

static GLint uniform_location_find(GLuint program, const GLchar *name)
{

	/* c[N], as the renderer probes c[]'s length after asking for c: the
virtual location's Nth, all 192 (its uploads go where each is) */
	if (program < LAYER_TRACKED && !strncmp(name, "c[", 2) && (constant_split[program] || lazy_pending(program)))
	{
		char *end;
		long index = strtol(name + 2, &end, 10);

		if (*end == ']' && index >= 0 && index < 192)
			return CONSTANTS_VIRTUAL_LOCATION + (GLint)index;
	}
	if (lazy_pending(program))
	{
		struct lazy_program *lazy = lazy_programs[program];
		int number, slot;

		if (!strcmp(name, "c"))
		{
			lazy->wants_constants = 1;
			return CONSTANTS_VIRTUAL_LOCATION;
		}
		number = lazy_name(name);
		for (slot = 0; number >= 0 && slot < lazy->count; slot++)
		{
			if (lazy->names[slot] == number)
				return LAZY_LOCATION + slot;
		}
		if (number >= 0 && lazy->count < LAZY_SLOTS)
		{
			lazy->names[lazy->count] = (unsigned char)number;
			return LAZY_LOCATION + lazy->count++;
		}
		/* (no room to keep it: loaded after all) */
		program_ready(program);
	}
	if (program >= LAYER_TRACKED || strcmp(name, "c"))
		return real_GetUniformLocation(program, name);
	return constants_split_build(program) ? CONSTANTS_VIRTUAL_LOCATION : -1;
}

/* a program's c[] found: where each constant went. Whether it has any */
static int constants_split_build(GLuint program)
{
	struct constant_split *split;
	int index, any;

	if (!constant_split[program])
		constant_split[program] = calloc(1, sizeof(struct constant_split));
	split = constant_split[program];
	if (!split)
		return 0;
	split->bones = real_GetUniformLocation(program, "c_bones");
	any = split->bones >= 0;
	for (index = 0; index < 192; index++)
	{
		char named[16];

		snprintf(named, sizeof(named), "c_%d", index);
		split->named[index] = real_GetUniformLocation(program, named);
		any |= split->named[index] >= 0;
	}
	if (getenv("HALO_GLES2_CONSTANT_LOG"))
	{
		static int logged;

		if (logged++ < 8)
			platform_log("gles2 constants: program %u: c_0 %d, c_58 %d, bones %d, %s", program, split->named[0],
				split->named[58], split->bones, any ? "split" : "none");
	}
	return any;
}

/* Vivante's GPU takes away what lies at a clip-space w under about 0.04
(found with a test triangle: w 0.02 drew 435 of its 1024 pixels, 0.01 none
at all), as if it clipped there and not at the near plane. The first-person
weapon's projection puts its near plane at 0.0234 (twice
rasterizer_first_person_weapon_near_clip_distance), and the assault rifle's
rear reaches past the camera plane: the box drew none of the part between,
the slab from its ammunition counter down to the screen's corner. A clip
position is homogeneous: the same one times any positive number is the
same point, depth and perspective-correct interpolation, so c[0] to c[3],
the only transform the shaders write oPos (and their projected texture
coordinates, divided by their own w) with, are uploaded times this, which
puts that edge at a 64th of the distance. The shaders read them nowhere
else. */
#define CLIP_SCALE 64.0f

/* HALO_GLES2_CONSTANT_LOG, -1 before it is looked up */
static int constant_log = -1;

static void GL_APIENTRY layer_Uniform4fv(GLint location, GLsizei count, const GLfloat *values)
{
	struct constant_split *split;
	GLint first, index;

	counts.uniforms++;
	if (experiment_bits & 256)
		return;
	if (location < CONSTANTS_VIRTUAL_LOCATION || location >= CONSTANTS_VIRTUAL_LOCATION + 192)
	{
		if (count != 1 && location >= 0 && program_renderer < LAYER_TRACKED && twins[program_renderer])
			LAYER_ONCE("a skinned program's uniform array: its flat twin keeps the first of it");
		location = lazy_location(twin_uniform(location, 4, values, 0));
		if (location >= 0)
			real_Uniform4fv(location, count, values);
		return;
	}
	program_ready(program_current);
	split = program_current < LAYER_TRACKED ? constant_split[program_current] : NULL;
	/* (looked up once: this is every upload, hundreds a frame, and getenv
	walks the whole environment) */
	if (constant_log < 0)
		constant_log = getenv("HALO_GLES2_CONSTANT_LOG") != NULL;
	if (constant_log)
	{
		static int logged;

		if (logged++ < 12)
			platform_log("gles2 constants: upload to program %u c[%d], %d of them (%g %g %g %g): %s", program_current,
				location - CONSTANTS_VIRTUAL_LOCATION, (int)count, values[0], values[1], values[2], values[3],
				split ? "split" : "no table");
	}
	if (!split)
		return;
	first = location - CONSTANTS_VIRTUAL_LOCATION;
	if (count > 192 - first)
		count = 192 - first;
	merge_stats_uniform(program_current, first, count, values);
	memcpy(split->kept[first], values, (size_t)count * 4 * sizeof(GLfloat));
	/* (past c[167]: kept for the draws that need them, bones_window_apply) */
	if (split->bones >= 0 && first + count > BONES_FIRST + BONES_COUNT)
		split->past_bones = 1;
	/* the block's, together, while it holds c[60] on (else the next draw
	that shifts it sends them, bones_window_apply) */
	if (split->bones >= 0 && first + count > BONES_FIRST && first < BONES_FIRST + BONES_COUNT && !split->window)
	{
		GLint from = first > BONES_FIRST ? first : BONES_FIRST;
		GLint to = first + count < BONES_FIRST + BONES_COUNT ? first + count : BONES_FIRST + BONES_COUNT;

		real_Uniform4fv(split->bones + (from - BONES_FIRST), to - from, values + (from - first) * 4);
	}
	else if (split->bones >= 0 && first + count > BONES_FIRST)
	{
		split->window_stale = 1;
	}
	/* and each named one (c[0] to c[3], the clip-space transform, scaled:
	CLIP_SCALE) */
	for (index = first; index < first + count; index++)
	{
		if (split->named[index] < 0)
			continue;
		if (index < 4)
		{
			GLfloat scaled[4];
			int component;

			for (component = 0; component < 4; component++)
				scaled[component] = values[(index - first) * 4 + component] * CLIP_SCALE;
			real_Uniform4fv(split->named[index], 1, scaled);
		}
		else
		{
			real_Uniform4fv(split->named[index], 1, values + (index - first) * 4);
		}
	}
}

/* a vertex_pruned_link program's position scale and offset, from the
values its uniforms hold (the renderer's, as for every program): the
screen-space conversion's c[58] and c[59] (x by the viewport's half size,
offset to its center less half a pixel) undone by the viewport's scale and
offset, as the shaders' own end does it */
static void position_uniforms_apply(void)
{
	static const char *const names[5] = { "viewport_scale", "viewport_offset", "c_58", "c_59", "screen_offset" };
	static void (GL_APIENTRY *get_uniform)(GLuint, GLint, GLfloat *);
	struct position_program *position = program_current < LAYER_TRACKED ? position_programs[program_current] : NULL;
	GLfloat values[17], scale[4], offset[4], divisor;
	int index;

	if (!position)
		return;
	if (!get_uniform)
		get_uniform = (__typeof__(get_uniform))SDL_GL_GetProcAddress("glGetUniformfv");
	if (!get_uniform)
		return;
	if (position->scale == -2)
	{
		position->scale = real_GetUniformLocation(program_current, "layer_position_scale");
		position->offset = real_GetUniformLocation(program_current, "layer_position_offset");
		for (index = 0; index < 5; index++)
			position->from[index] = real_GetUniformLocation(program_current, names[index]);
	}
	if (position->scale < 0 || position->offset < 0)
		return;
	memset(values, 0, sizeof(values));
	for (index = 0; index < 5; index++)
	{
		if (position->from[index] >= 0)
			get_uniform(program_current, position->from[index], values + index * 4);
	}
	if (position->given && !memcmp(values, position->last, sizeof(values)))
		return;
	memcpy(position->last, values, sizeof(values));
	position->given = 1;
	for (index = 0; index < 3; index++)
	{
		static const GLfloat half_pixel[3] = { 0.5f, 0.5f, 0.0f };

		divisor = values[index] != 0.0f ? values[index] : 1.0f;
		scale[index] = values[8 + index] / divisor;
		offset[index] = (values[12 + index] + half_pixel[index] + (index == 0 ? values[16] : 0.0f) - values[4 + index]) /
			divisor;
	}
	scale[3] = offset[3] = 0.0f;
	real_Uniform4fv(position->scale, 1, scale);
	real_Uniform4fv(position->offset, 1, offset);
}

/* an attribute's component as the GPU reads it, a float */
static GLfloat attribute_component(const struct layer_attribute *attribute, const unsigned char *vertex, int component)
{
	switch (attribute->type)
	{
	case GL_FLOAT:
	{
		GLfloat value;

		memcpy(&value, vertex + component * 4, sizeof(value));
		return value;
	}
	case GL_SHORT:
	{
		GLshort value;

		memcpy(&value, vertex + component * 2, sizeof(value));
		return attribute->normalized ? (value < -32767 ? -1.0f : (GLfloat)value / 32767.0f) : (GLfloat)value;
	}
	case GL_UNSIGNED_SHORT:
	{
		GLushort value;

		memcpy(&value, vertex + component * 2, sizeof(value));
		return attribute->normalized ? (GLfloat)value / 65535.0f : (GLfloat)value;
	}
	case GL_BYTE:
		return attribute->normalized ? ((GLbyte)vertex[component] < -127 ? -1.0f :
			(GLfloat)(GLbyte)vertex[component] / 127.0f) : (GLfloat)(GLbyte)vertex[component];
	case GL_UNSIGNED_BYTE:
		return attribute->normalized ? (GLfloat)vertex[component] / 255.0f : (GLfloat)vertex[component];
	default:
		return 0.0f;
	}
}

/* whether the current program is a skinned one that has been given nodes
past c_bones (or holds a window of them now) */
static int bones_window_needed(void)
{
	const struct constant_split *split = program_current < LAYER_TRACKED ? constant_split[program_current] : NULL;

	return split && split->bones >= 0 && (split->past_bones || split->window);
}

/* A skinned model with more nodes than c_bones holds (the first-person
weapons: the Xbox had room to c[191], 44 nodes; the box's shaders have 36,
to c[167]). A draw seldom reaches nodes far apart, so the 36 its vertices
need go into c_bones (from layer_Uniform4fv's copy of c[]), and its node
indices are given less that much: a copy of them, as for the packed
normals. Every other draw, c_bones holds c[60] on as before. Vertices
lowest to highest are the draw's */
/* a skinned draw's rows and shifted node indices, kept: a model's draws are
the same every frame (the first-person weapon's every one), and their scan
was 2% of the game's time. Known by where the indices are, which vertices,
the scale and a few of the indices' bytes, as the memory may be another
model's later */
#define BONES_SCANS 16

static struct bones_scan
{
	const unsigned char *source, *weights;
	GLsizei stride;
	unsigned long lowest, highest;
	GLfloat scale;
	unsigned long sample;
	long low, high;
	/* the indices less the window, for window (0: none made) */
	long shifted_window;
	GLfloat *shifted;
} bones_scans[BONES_SCANS];
static int bones_scan_next;

static unsigned long bones_scan_sample(const unsigned char *source, GLsizei stride, unsigned long lowest,
	unsigned long highest)
{
	unsigned long middle = lowest + (highest - lowest) / 2, sample = 0;

	memcpy(&sample, source + lowest * (unsigned long)stride, 2);
	sample = sample * 31 + source[middle * (unsigned long)stride] + (source[middle * (unsigned long)stride + 1] << 8);
	sample = sample * 31 + source[highest * (unsigned long)stride] + (source[highest * (unsigned long)stride + 1] << 8);
	return sample;
}

static void bones_window_apply(unsigned long lowest, unsigned long highest)
{
	struct bones_scan *scan = NULL;
	unsigned long sample;
	int index;
	struct constant_split *split = program_current < LAYER_TRACKED ? constant_split[program_current] : NULL;
	const struct layer_shader *vertex_shader;
	struct layer_attribute *attribute;
	const struct layer_attribute *weight_attribute = NULL;
	const unsigned char *source, *weights = NULL;
	GLsizei weight_stride = 0;
	const long *byte_rows = NULL;
	GLfloat scale;
	long low = 0x7fffffff, high = -1, window = 0;
	unsigned long vertex;
	GLsizei stride;
	int components, component;

	if (!split || split->bones < 0 || (!split->past_bones && !split->window) || highest < lowest)
		return;
	vertex_shader = program_vertex_shaders[program_current] < LAYER_TRACKED ?
		&shaders[program_vertex_shaders[program_current]] : NULL;
	if (!vertex_shader || vertex_shader->node_attribute < 0 || vertex_shader->node_attribute >= LAYER_ATTRIBUTES ||
		vertex_shader->node_attribute >= attribute_maximum || vertex_shader->node_scale < 0 ||
		vertex_shader->node_scale >= 192)
	{
		return;
	}
	attribute = &vertex_array()->attributes[vertex_shader->node_attribute];
	source = buffer_pointer(attribute->buffer, attribute->pointer);
	scale = split->kept[vertex_shader->node_scale][3];
	components = attribute->size > 0 && attribute->size <= 4 ? attribute->size : 4;
	stride = attribute->stride ? attribute->stride : components * (attribute->type == GL_FLOAT ? 4 :
		attribute->type == GL_SHORT || attribute->type == GL_UNSIGNED_SHORT ? 2 : 1);
	if (!source || scale <= 0.0f)
		return;
	if (vertex_shader->weight_attribute >= 0 && vertex_shader->weight_attribute < LAYER_ATTRIBUTES &&
		vertex_shader->weight_attribute < attribute_maximum)
	{
		const struct layer_buffer *buffer;

		weight_attribute = &vertex_array()->attributes[vertex_shader->weight_attribute];
		weights = buffer_pointer(weight_attribute->buffer, weight_attribute->pointer);
		weight_stride = weight_attribute->stride ? weight_attribute->stride :
			(weight_attribute->size > 0 ? weight_attribute->size : 4) * (weight_attribute->type == GL_FLOAT ? 4 :
			weight_attribute->type == GL_SHORT || weight_attribute->type == GL_UNSIGNED_SHORT ? 2 : 1);
		buffer = buffer_get(weight_attribute->buffer);
		if (buffer && buffer->size >= 0 &&
			(unsigned long)(uintptr_t)weight_attribute->pointer + highest * (unsigned long)weight_stride + 16 >
			(unsigned long)buffer->size)
		{
			weights = NULL;
		}
	}
	{
		const struct layer_buffer *buffer = buffer_get(attribute->buffer);

		if (buffer && buffer->size >= 0 &&
			(unsigned long)(uintptr_t)attribute->pointer + highest * (unsigned long)stride + (unsigned long)components * 4 >
			(unsigned long)buffer->size)
		{
			return;
		}
	}
	sample = bones_scan_sample(source, stride, lowest, highest);
	for (index = 0; index < BONES_SCANS && !scan; index++)
	{
		struct bones_scan *kept = &bones_scans[index];

		if (kept->source == source && kept->weights == weights && kept->stride == stride &&
			kept->lowest == lowest && kept->highest == highest && kept->scale == scale && kept->sample == sample)
		{
			scan = kept;
		}
	}
	if (scan)
	{
		low = scan->low;
		high = scan->high;
		goto scanned;
	}
	if (attribute->type == GL_UNSIGNED_BYTE && attribute->normalized)
	{
		static long table[256];
		static GLfloat table_scale;
		int byte;

		if (table_scale != scale)
		{
			for (byte = 0; byte < 256; byte++)
				table[byte] = (long)floorf((GLfloat)byte / 255.0f * scale + 0.001f);
			table_scale = scale;
		}
		byte_rows = table;
	}
	/* the rows the draw's vertices read: node * 3 to node * 3 + 2, each of
	the two nodes a vertex has (as the shader takes them, a0) */
	for (vertex = lowest; vertex <= highest; vertex++)
	{
		const unsigned char *at = source + vertex * (unsigned long)stride;
		/* (the second node only where it weighs something: a vertex of one
		node has filler there) */
		int nodes = 2;

		if (weights)
		{
			const unsigned char *weight = weights + vertex * (unsigned long)weight_stride;
			GLfloat first_weight = attribute_component(weight_attribute, weight, 0);
			GLfloat sum = weight_attribute->size >= 4 ? attribute_component(weight_attribute, weight, 3) : 1.0f;

			if (fabsf(sum - first_weight) < 0.0001f)
				nodes = 1;
		}
		for (component = 0; component < nodes && component < components; component++)
		{
			/* (the models' node indices are normalized bytes: a table) */
			long row = byte_rows ? byte_rows[at[component]] :
				(long)floorf(attribute_component(attribute, at, component) * scale + 0.001f);

			if (row < low)
				low = row;
			if (row + 2 > high)
				high = row + 2;
		}
	}
	scan = &bones_scans[bones_scan_next];
	bones_scan_next = (bones_scan_next + 1) % BONES_SCANS;
	free(scan->shifted);
	memset(scan, 0, sizeof(*scan));
	scan->source = source;
	scan->weights = weights;
	scan->stride = stride;
	scan->lowest = lowest;
	scan->highest = highest;
	scan->scale = scale;
	scan->sample = sample;
	scan->low = low;
	scan->high = high;
scanned:
	if (high >= BONES_COUNT)
	{
		if (high - low >= BONES_COUNT || low < 0)
		{
			LAYER_ONCE("a skinned draw reaches nodes further apart than c_bones holds (rows %ld to %ld)", low, high);
			return;
		}
		window = low;
		LAYER_ONCE("skinned draws past c[%d] given the nodes they need in c_bones", BONES_FIRST + BONES_COUNT - 1);
	}
	if (window != split->window || (window && split->window_stale))
	{
		GLsizei rows = BONES_FIRST + window + BONES_COUNT <= 192 ? BONES_COUNT : 192 - BONES_FIRST - (GLsizei)window;

		real_Uniform4fv(split->bones, rows, split->kept[BONES_FIRST + window]);
		split->window = (int)window;
		split->window_stale = 0;
	}
	if (!window)
	{
		/* (the renderer's own node indices again, should a shifted copy have
		taken their place) */
		attribute_apply((GLuint)vertex_shader->node_attribute, attribute);
		return;
	}
	if (!scan->shifted || scan->shifted_window != window)
	{
		free(scan->shifted);
		scan->shifted = malloc((highest - lowest + 1) * 4 * sizeof(GLfloat));
		if (!scan->shifted)
			return;
		for (vertex = lowest; vertex <= highest; vertex++)
		{
			const unsigned char *at = source + vertex * (unsigned long)stride;
			GLfloat *to = scan->shifted + (vertex - lowest) * 4;

			for (component = 0; component < 4; component++)
			{
				to[component] = component < components ? attribute_component(attribute, at, component) :
					(component == 3 ? 1.0f : 0.0f);
				if (component < 2)
					to[component] -= (GLfloat)window / scale;
			}
		}
		scan->shifted_window = window;
	}
	counts.pointers++;
	real.BindBuffer(GL_ARRAY_BUFFER, 0);
	real.VertexAttribPointer((GLuint)vertex_shader->node_attribute, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
		scan->shifted - lowest * 4);
}

/* HALO_GLES2_SKIN_LOG: the first skinned draws' attributes (their formats
and the first vertex), and the constants their nodes pick */
static void skin_log(GLsizei count, GLenum type, const void *indices)
{
	static int logged = -1;
	static void (GL_APIENTRY *get_uniform)(GLuint, GLint, GLfloat *);
	const struct layer_vertex_array *current = vertex_array();
	GLuint index;
	unsigned int first;

	if (logged < 0)
		logged = getenv("HALO_GLES2_SKIN_LOG") ? 0 : 1000;
	if (logged >= 6 || !program_relative(program_current) || !indices)
		return;
	logged++;
	first = type == GL_UNSIGNED_SHORT ? ((const GLushort *)indices)[0] : ((const GLuint *)indices)[0];
	platform_log("gles2 skin: program %u, %d indices, the first %u", program_current, (int)count, first);
	for (index = 0; index < (GLuint)attribute_maximum; index++)
	{
		const struct layer_attribute *attribute = &current->attributes[index];
		const unsigned char *data = buffer_pointer(attribute->buffer, attribute->pointer);
		char bytes[64];
		size_t length = 0, byte;

		if (!(current->enabled & (1u << index)) || !data)
			continue;
		data += (size_t)first * (size_t)attribute->stride;
		for (byte = 0; byte < 12 && length + 4 < sizeof(bytes); byte++)
			length += (size_t)snprintf(bytes + length, sizeof(bytes) - length, "%02x", data[byte]);
		platform_log("gles2 skin:   attribute %u: %d x 0x%x%s, stride %d, offset %lu, bytes %s", index,
			(int)attribute->size, (unsigned)attribute->type, attribute->normalized ? " normalized" : "",
			(int)attribute->stride, (unsigned long)(uintptr_t)attribute->pointer, bytes);
	}
	if (!get_uniform)
		get_uniform = (__typeof__(get_uniform))SDL_GL_GetProcAddress("glGetUniformfv");
	{
		static const int wanted[] = { 7, 60, 61, 62, 167, 168, 190, 191 };
		unsigned k;

		for (k = 0; get_uniform && k < sizeof(wanted) / sizeof(*wanted); k++)
		{
			GLfloat value[4] = { 0 };
			char name[16];

			snprintf(name, sizeof(name), "c[%d]", wanted[k]);
			get_uniform(program_current, real_GetUniformLocation(program_current, name), value);
			platform_log("gles2 skin:   c[%d] %g %g %g %g", wanted[k], value[0], value[1], value[2], value[3]);
		}
	}
}

/* ---------- flat twins: the first-person models' normals in performance mode

Real normals cost the first-person weapon's and arms' skinned shaders 3 to
4 ms a frame on damnation (the normal, binormal and tangent each skinned by
two nodes, which with every normal straight up the compiler leaves out). In
performance mode a skinned program's draws with the first-person
projection (draw_first_person) draw with its flat twin instead: the same
program linked with NORMPACKED3_FLAT. The renderer knows only its own
program: while the twin stands in for it, its uniforms (by name, from the
renderer's own lookups) and its c[] go to the twin, and on each change of
which one draws, the one taking over is given everything the other had.
Quality mode, the level and every other model keep their real normals;
HALO_GLES2_FIRST_PERSON_NORMALS keeps them in performance mode too. */

#define TWIN_UNIFORMS 64

struct twin
{
	/* the flat twin (0: not made yet), and whether it is drawing in the
	renderer's program's place */
	GLuint program;
	int failed, active;
	int count;
	struct
	{
		GLint location, twin_location;
		char name[48];
		int kind, given;
		GLfloat values[4];
		GLint integer;
	} uniforms[TWIN_UNIFORMS];
};

static unsigned char program_is_twin[LAYER_TRACKED];
static int twin_replaying;
/* whether performance mode is on, asked at each present (-1: not yet) */
static int twin_performance = -1;
long console_performance_lines(void);

static int twin_wanted_mode(void)
{
	static int forced = -1;

	if (forced < 0)
		forced = getenv("HALO_GLES2_FIRST_PERSON_NORMALS") != NULL;
	if (forced || normals_flat())
		return 0;
	if (twin_performance < 0)
		twin_performance = console_performance_lines() != 0;
	return twin_performance;
}

/* at each present: performance mode switches in the game */
static void twin_frame(void)
{
	twin_performance = console_performance_lines() != 0;
}

/* whether the draw's c[2] and c[3] (clip z and w) put its near plane where
the first-person weapon's projection does (0.0117 to the level's 0.0625,
rasterizer.c's defaults: under 0.05, whichever depth range the projection
has) */
static int draw_first_person(void)
{
	const struct constant_split *split = program_current < LAYER_TRACKED ? constant_split[program_current] : NULL;
	const GLfloat *z, *w;
	GLfloat a, b, near;
	int axis = 0, index;

	if (!split)
		return 0;
	z = split->kept[2];
	w = split->kept[3];
	for (index = 1; index < 3; index++)
	{
		if (fabsf(w[index]) > fabsf(w[axis]))
			axis = index;
	}
	if (fabsf(w[axis]) < 1.0e-6f)
		return 0;
	a = z[axis] / w[axis];
	if (fabsf(a) < 1.0e-6f)
		return 0;
	b = z[3] - a * w[3];
	near = -b / a;
	if (getenv("HALO_GLES2_NORMALS_LOG"))
	{
		static GLfloat seen[8];
		static int seen_count;
		int known = 0;

		for (index = 0; index < seen_count; index++)
			known |= fabsf(seen[index] - near) < 0.001f;
		if (!known && seen_count < 8)
		{
			seen[seen_count++] = near;
			platform_log("gles2 normals: a skinned draw's near plane %g (%s)", near,
				near > 0.0f && near < 0.05f ? "first person" : "not first person");
		}
	}
	return near > 0.0f && near < 0.05f;
}

/* a renderer's location of a skinned program, by name (layer_GetUniformLocation) */
static void twin_name(GLuint program, const GLchar *name, GLint location)
{
	struct twin *twin;
	int index;

	if (program >= LAYER_TRACKED || program_is_twin[program] || location < 0 ||
		(location >= CONSTANTS_VIRTUAL_LOCATION && location < CONSTANTS_VIRTUAL_LOCATION + 192) ||
		strlen(name) >= sizeof(twin->uniforms[0].name) || !program_relative(program))
	{
		return;
	}
	if (!twins[program])
		twins[program] = calloc(1, sizeof(struct twin));
	twin = twins[program];
	if (!twin)
		return;
	for (index = 0; index < twin->count; index++)
	{
		if (twin->uniforms[index].location == location)
			return;
	}
	if (twin->count >= TWIN_UNIFORMS)
	{
		LAYER_ONCE("a skinned program with more uniforms than its flat twin keeps");
		return;
	}
	twin->uniforms[twin->count].location = location;
	twin->uniforms[twin->count].twin_location = -2;
	strcpy(twin->uniforms[twin->count].name, name);
	twin->count++;
}

/* a uniform the renderer gives its current program: kept, and the twin's
location for it while the twin draws (1: an integer, 4: up to four floats) */
static GLint twin_uniform(GLint location, int kind, const GLfloat *values, GLint integer)
{
	struct twin *twin = program_renderer < LAYER_TRACKED ? twins[program_renderer] : NULL;
	int index;

	if (!twin || twin_replaying || location < 0 ||
		(location >= CONSTANTS_VIRTUAL_LOCATION && location < CONSTANTS_VIRTUAL_LOCATION + 192))
	{
		return location;
	}
	for (index = 0; index < twin->count; index++)
	{
		if (twin->uniforms[index].location != location)
			continue;
		twin->uniforms[index].kind = kind;
		twin->uniforms[index].given = 1;
		twin->uniforms[index].integer = integer;
		if (values)
			memcpy(twin->uniforms[index].values, values, sizeof(twin->uniforms[index].values));
		if (!twin->active || !twin->program)
			return location;
		if (twin->uniforms[index].twin_location == -2)
			twin->uniforms[index].twin_location = layer_GetUniformLocation(twin->program, twin->uniforms[index].name);
		return twin->uniforms[index].twin_location;
	}
	return twin->active ? -1 : location;
}

static void GL_APIENTRY layer_Uniform1i(GLint location, GLint value);
static void GL_APIENTRY layer_Uniform1f(GLint location, GLfloat x);
static void GL_APIENTRY layer_Uniform2f(GLint location, GLfloat x, GLfloat y);

/* the flat twin of a skinned program: whether it was made */
static int twin_build(GLuint program, struct twin *twin)
{
	static const char *const pairs[] = { NORMPACKED3_UNPACK, NORMPACKED3_FLAT, NULL };
	static GLuint (GL_APIENTRY *create_program)(void);
	GLuint vertex = program_vertex_shaders[program], fragment = program_fragment_shaders[program], flat_shader, made;
	struct text flat = { 0 };
	const GLchar *text;
	GLint linked = 0;
	GLuint current = program_current;

	twin->failed = 1;
	if (!vertex || vertex >= LAYER_TRACKED || !fragment || !shaders[vertex].source ||
		!strstr(shaders[vertex].source, NORMPACKED3_UNPACK))
	{
		return 0;
	}
	if (!create_program)
		create_program = (__typeof__(create_program))SDL_GL_GetProcAddress("glCreateProgram");
	if (!create_program)
		return 0;
	replace_text(&flat, shaders[vertex].source, pairs);
	if (!flat.buffer)
		return 0;
	flat_shader = layer_CreateShader(GL_VERTEX_SHADER);
	if (!flat_shader || flat_shader >= LAYER_TRACKED)
	{
		free(flat.buffer);
		return 0;
	}
	{
		struct layer_shader copy = shaders[vertex];

		copy.source = flat.buffer;
		copy.deferred = 0;
		shaders[flat_shader] = copy;
	}
	text = flat.buffer;
	real.ShaderSource(flat_shader, 1, &text, NULL);
	real.CompileShader(flat_shader);
	counts.compiles++;
	made = create_program();
	if (!made || made >= LAYER_TRACKED)
		return 0;
	program_is_twin[made] = 1;
	layer_AttachShader(made, flat_shader);
	layer_AttachShader(made, fragment);
	layer_LinkProgram(made);
	program_ready(made);
	halo_glGetProgramiv(made, GL_LINK_STATUS, &linked);
	platform_log("gles2 normals: program %u's flat twin, for its first-person draws in performance mode: %s", program,
		linked ? "linked" : "does not link");
	if (!linked)
		return 0;
	/* (its c[] found, as the renderer's lookup of "c" does) */
	layer_GetUniformLocation(made, "c");
	program_current = current;
	twin->program = made;
	twin->failed = 0;
	return 1;
}

/* the renderer's program or its twin made the one drawing, given all the
other had */
static void twin_switch(struct twin *twin, GLuint from, GLuint to, int to_twin)
{
	const struct constant_split *split = from < LAYER_TRACKED ? constant_split[from] : NULL;
	int index;

	program_current = to;
	program_ready(to);
	real.UseProgram(to);
	twin_replaying = 1;
	if (split)
	{
		GLfloat kept[192][4];

		memcpy(kept, split->kept, sizeof(kept));
		layer_Uniform4fv(CONSTANTS_VIRTUAL_LOCATION, 192, kept[0]);
	}
	for (index = 0; index < twin->count; index++)
	{
		GLint location = twin->uniforms[index].location;

		if (!twin->uniforms[index].given)
			continue;
		if (to_twin)
		{
			if (twin->uniforms[index].twin_location == -2)
				twin->uniforms[index].twin_location = layer_GetUniformLocation(to, twin->uniforms[index].name);
			location = twin->uniforms[index].twin_location;
		}
		if (location < 0)
			continue;
		switch (twin->uniforms[index].kind)
		{
		case 1: layer_Uniform1i(location, twin->uniforms[index].integer); break;
		case 2: layer_Uniform1f(location, twin->uniforms[index].values[0]); break;
		case 3: layer_Uniform2f(location, twin->uniforms[index].values[0], twin->uniforms[index].values[1]); break;
		case 4: layer_Uniform4fv(location, 1, twin->uniforms[index].values); break;
		default: break;
		}
	}
	twin_replaying = 0;
	twin->active = to_twin;
}

/* before each draw: the renderer's skinned program or its flat twin, as the
draw wants */
static void twin_draw_begin(void)
{
	struct twin *twin = program_renderer && program_renderer < LAYER_TRACKED ? twins[program_renderer] : NULL;
	int want;

	if (!twin || (!twin->active && !twin_wanted_mode()))
		return;
	program_ready(program_current);
	want = twin_wanted_mode() && draw_first_person();
	if (want == twin->active)
		return;
	if (want && !twin->program && (twin->failed || !twin_build(program_renderer, twin)))
		return;
	if (want)
		twin_switch(twin, program_renderer, twin->program, 1);
	else
		twin_switch(twin, twin->program, program_renderer, 0);
}

/* a program linked again is another: its twin forgotten */
static void twin_forget(GLuint program)
{
	static void (GL_APIENTRY *delete_program)(GLuint);

	if (program >= LAYER_TRACKED || !twins[program])
		return;
	if (twins[program]->program)
	{
		if (!delete_program)
			delete_program = (__typeof__(delete_program))SDL_GL_GetProcAddress("glDeleteProgram");
		if (delete_program)
			delete_program(twins[program]->program);
		program_is_twin[twins[program]->program] = 0;
	}
	free(twins[program]);
	twins[program] = NULL;
}

static void GL_APIENTRY layer_UseProgram(GLuint program)
{
	program_renderer = program;
	/* (its flat twin, while that draws in its place) */
	if (program < LAYER_TRACKED && twins[program] && twins[program]->active && twins[program]->program)
		program = twins[program]->program;
	program_current = program;
	counts.programs++;
	if (!lazy_pending(program))
		real.UseProgram(program);
}

/* ---------- the Android host's GL helpers (xgpu.h) */

int host_gl_has_extension(const char *name)
{
	const char *extensions = (const char *)halo_glGetString(GL_EXTENSIONS);
	size_t length = strlen(name);
	const char *at = extensions;

	while (at && (at = strstr(at, name)) != NULL)
	{
		if ((at == extensions || at[-1] == ' ') && (at[length] == ' ' || at[length] == 0))
			return 1;
		at += length;
	}
	return 0;
}

void host_gl_read_buffer(unsigned int buffer, unsigned int offset, unsigned int size, void *data)
{
	(void)buffer;
	(void)offset;
	/* (ES 2 cannot read a buffer back) */
	memset(data, 0, size);
}

void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data)
{
	halo_glBufferSubData(target, (GLintptr)offset, (GLsizeiptr)size, data);
}

/* (no fences in ES 2: the driver orders a buffer's writes after the draws
that read it) */
void host_gl_fence_frame(unsigned int slot)
{
	(void)slot;
}

void host_gl_wait_frame(unsigned int slot)
{
	(void)slot;
}

/* ---------- a sampling profiler (HALO_GLES2_PROFILE)

The box has no perf and no gdb: a SIGPROF 100 times a second of the
process's time records the program counter it interrupted (and the link
register, for code in a library), and every 10 seconds the most common go
to the log, for llvm-symbolizer against the unstripped executable. */

#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <ucontext.h>

#define PROFILE_SLOTS 65536

static volatile unsigned int profile_pcs[PROFILE_SLOTS], profile_counts[PROFILE_SLOTS];
static volatile unsigned int profile_total;

static void profile_record(unsigned int pc)
{
	unsigned int slot = (pc >> 2) % PROFILE_SLOTS, probe;

	for (probe = 0; probe < 16; probe++, slot = (slot + 1) % PROFILE_SLOTS)
	{
		if (profile_pcs[slot] == pc || !profile_pcs[slot])
		{
			profile_pcs[slot] = pc;
			profile_counts[slot]++;
			return;
		}
	}
}

static void profile_signal(int signal_number, siginfo_t *information, void *context)
{
	ucontext_t *ucontext = context;

	(void)signal_number;
	(void)information;
	profile_record((unsigned int)ucontext->uc_mcontext.arm_pc);
	/* (the caller too, as the pc | 1: a library's own pc says little) */
	profile_record((unsigned int)ucontext->uc_mcontext.arm_lr | 1u);
	profile_total++;
}

static void profile_report(void)
{
	static Uint64 last;
	Uint64 now = SDL_GetTicks();
	unsigned int best[30], best_count[30], slot, index, total = profile_total;

	if (now - last < 9000)
		return;
	last = now;
	memset(best_count, 0, sizeof(best_count));
	memset(best, 0, sizeof(best));
	for (slot = 0; slot < PROFILE_SLOTS; slot++)
	{
		unsigned int count = profile_counts[slot];

		for (index = 0; index < 30; index++)
		{
			if (count > best_count[index])
			{
				memmove(&best[index + 1], &best[index], (29 - index) * sizeof(*best));
				memmove(&best_count[index + 1], &best_count[index], (29 - index) * sizeof(*best_count));
				best[index] = profile_pcs[slot];
				best_count[index] = count;
				break;
			}
		}
	}
	/* (HALO_GLES2_PROFILE=file: every pc of the window there, for a profile
by function off the box) */
	{
		const char *path = getenv("HALO_GLES2_PROFILE");
		FILE *file = path && strcmp(path, "1") ? fopen(path, "w") : NULL;

		if (file)
		{
			/* (where each library is, for the pcs: once, beside the file) */
			static int mapped;
			char maps_path[1100], text[4096];
			FILE *maps = mapped ? NULL : fopen("/proc/self/maps", "r"), *copy;
			size_t length;

			snprintf(maps_path, sizeof(maps_path), "%s.maps", path);
			if (maps && (copy = fopen(maps_path, "w")))
			{
				while ((length = fread(text, 1, sizeof(text), maps)) > 0)
					fwrite(text, 1, length, copy);
				fclose(copy);
				mapped = 1;
			}
			if (maps)
				fclose(maps);
			fprintf(file, "samples %u\n", total);
			for (slot = 0; slot < PROFILE_SLOTS; slot++)
			{
				if (profile_counts[slot])
					fprintf(file, "%08x %u\n", profile_pcs[slot], profile_counts[slot]);
			}
			fclose(file);
		}
	}
	/* (the last ten seconds only: counted again from nothing) */
	platform_log("gles2 profile: %u samples", total);
	for (index = 0; index < 30 && best_count[index]; index++)
		platform_log("gles2 profile: %08x %u", best[index], best_count[index]);
	memset((void *)profile_counts, 0, sizeof(profile_counts));
	memset((void *)profile_pcs, 0, sizeof(profile_pcs));
	profile_total = 0;
}

/* (its own thread: the game's may be the one stuck) */
static void *profile_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		sleep(10);
		profile_report();
	}
	return NULL;
}

/* a bus error (a misaligned access the CPU will not do: VFP loads, ldrd):
where, in the log, before the signal goes on as it would have (the game's
crash handler takes SIGSEGV only) */
static void bus_error(int signal_number, siginfo_t *information, void *context)
{
	ucontext_t *ucontext = context;
	char line[160];
	int length;

	length = snprintf(line, sizeof(line), "gles2: bus error at %p, pc %08lx lr %08lx\n", information->si_addr,
		(unsigned long)ucontext->uc_mcontext.arm_pc, (unsigned long)ucontext->uc_mcontext.arm_lr);
	if (write(STDERR_FILENO, line, (size_t)length) < 0)
		length = 0;
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

static void bus_error_watch(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = bus_error;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGBUS, &action, NULL);
}

static void profile_start(void)
{
	pthread_t thread;
	struct sigaction action;
	struct itimerval timer;

	if (!getenv("HALO_GLES2_PROFILE"))
		return;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = profile_signal;
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGPROF, &action, NULL);
	timer.it_interval.tv_sec = 0;
	timer.it_interval.tv_usec = 10000;
	timer.it_value = timer.it_interval;
	setitimer(ITIMER_PROF, &timer, NULL);
	pthread_create(&thread, NULL, profile_thread, NULL);
	platform_log("gles2: profiling");
}

/* ---------- the layer */

#define LAYER_SAVE(name) real.name = halo_gl##name
#define LAYER_REPLACE(name) halo_gl##name = layer_##name

/* The layer binds a GPU buffer for each attribute pointer and indexed draw
from one; the driver's binding is kept, and the same again not passed on
(HALO_GLES2_NO_STATE_CACHE, not kept). */
static struct
{
	__typeof__(halo_glBindBuffer) BindBuffer;
	GLuint array_buffer, element_buffer;
} driver;

static void GL_APIENTRY driver_BindBuffer(GLenum target, GLuint buffer)
{
	if (target == GL_ARRAY_BUFFER)
	{
		if (buffer == driver.array_buffer)
			return;
		driver.array_buffer = buffer;
	}
	else if (target == GL_ELEMENT_ARRAY_BUFFER)
	{
		if (buffer == driver.element_buffer)
			return;
		driver.element_buffer = buffer;
	}
	driver.BindBuffer(target, buffer);
}

int console_gles2_layer(void)
{
	GLint unit;

	LAYER_SAVE(GetIntegerv);
	LAYER_SAVE(ActiveTexture);
	LAYER_SAVE(BindTexture);
	LAYER_SAVE(TexParameteri);
	LAYER_SAVE(TexParameteriv);
	LAYER_SAVE(TexParameterf);
	LAYER_SAVE(TexParameterfv);
	LAYER_SAVE(TexImage2D);
	LAYER_SAVE(TexSubImage2D);
	LAYER_SAVE(CompressedTexImage2D);
	LAYER_SAVE(BindFramebuffer);
	LAYER_SAVE(FramebufferTexture2D);
	LAYER_SAVE(FramebufferRenderbuffer);
	LAYER_SAVE(CheckFramebufferStatus);
	LAYER_SAVE(ReadPixels);
	LAYER_SAVE(BindBuffer);
	LAYER_SAVE(EnableVertexAttribArray);
	LAYER_SAVE(DisableVertexAttribArray);
	LAYER_SAVE(VertexAttribPointer);
	if (real.BindBuffer && !getenv("HALO_GLES2_NO_STATE_CACHE"))
	{
		driver.BindBuffer = real.BindBuffer;
		real.BindBuffer = driver_BindBuffer;
	}
	LAYER_SAVE(CreateShader);
	LAYER_SAVE(ShaderSource);
	LAYER_SAVE(CompileShader);
	LAYER_SAVE(AttachShader);
	LAYER_SAVE(LinkProgram);
	LAYER_SAVE(UseProgram);
	real_GenTextures = halo_glGenTextures;
	real_DeleteTextures = halo_glDeleteTextures;
	real.RenderbufferStorage = (__typeof__(real.RenderbufferStorage))SDL_GL_GetProcAddress("glRenderbufferStorage");
	real.GetFramebufferAttachmentParameteriv = (__typeof__(real.GetFramebufferAttachmentParameteriv))
		SDL_GL_GetProcAddress("glGetFramebufferAttachmentParameteriv");
	real.VertexAttrib4f = (__typeof__(real.VertexAttrib4f))SDL_GL_GetProcAddress("glVertexAttrib4f");
	real_CopyTexSubImage2D = (__typeof__(real_CopyTexSubImage2D))SDL_GL_GetProcAddress("glCopyTexSubImage2D");
	real.DiscardFramebufferEXT = host_gl_has_extension("GL_EXT_discard_framebuffer") ?
		(__typeof__(real.DiscardFramebufferEXT))SDL_GL_GetProcAddress("glDiscardFramebufferEXT") : NULL;
	if (!real.GetIntegerv || !real.ActiveTexture || !real.BindTexture || !real.TexParameteri || !real.TexImage2D ||
		!real.BindFramebuffer || !real.FramebufferTexture2D || !real.BindBuffer || !real.VertexAttribPointer ||
		!real.CreateShader || !real.ShaderSource || !real.CompileShader || !real.LinkProgram ||
		!real.RenderbufferStorage || !real.GetFramebufferAttachmentParameteriv || !real.VertexAttrib4f ||
		!real_GenTextures || !real_DeleteTextures)
	{
		platform_log("gles2: the driver lacks an ES 2 entry point");
		return 0;
	}

	LAYER_REPLACE(GetIntegerv);
	LAYER_REPLACE(ActiveTexture);
	LAYER_REPLACE(BindTexture);
	LAYER_REPLACE(GenTextures);
	halo_glDeleteTextures = layer_DeleteTextures_hook;
	LAYER_REPLACE(TexParameteri);
	LAYER_REPLACE(TexParameteriv);
	LAYER_REPLACE(TexParameterf);
	LAYER_REPLACE(TexParameterfv);
	LAYER_REPLACE(TexImage2D);
	LAYER_REPLACE(TexSubImage2D);
	LAYER_REPLACE(CompressedTexImage2D);
	LAYER_REPLACE(TexImage3D);
	LAYER_REPLACE(CompressedTexImage3D);
	LAYER_REPLACE(GenSamplers);
	LAYER_REPLACE(BindSampler);
	LAYER_REPLACE(SamplerParameteri);
	LAYER_REPLACE(SamplerParameterf);
	LAYER_REPLACE(SamplerParameterfv);
	LAYER_REPLACE(BindFramebuffer);
	LAYER_REPLACE(FramebufferTexture2D);
	LAYER_REPLACE(FramebufferRenderbuffer);
	LAYER_REPLACE(CheckFramebufferStatus);
	LAYER_REPLACE(ReadPixels);
	LAYER_REPLACE(BlitFramebuffer);
	LAYER_REPLACE(DrawBuffers);
	LAYER_REPLACE(ReadBuffer);
	LAYER_REPLACE(InvalidateFramebuffer);
	LAYER_REPLACE(RenderbufferStorageMultisample);
	LAYER_REPLACE(BindBuffer);
	real_GenBuffers = halo_glGenBuffers;
	real_BufferData = halo_glBufferData;
	real_BufferSubData = halo_glBufferSubData;
	LAYER_REPLACE(GenBuffers);
	LAYER_REPLACE(DeleteBuffers);
	LAYER_REPLACE(BufferData);
	LAYER_REPLACE(BufferSubData);
	LAYER_REPLACE(CopyBufferSubData);
	LAYER_REPLACE(BindBufferBase);
	LAYER_REPLACE(BindBufferRange);
	LAYER_REPLACE(GenVertexArrays);
	LAYER_REPLACE(BindVertexArray);
	LAYER_REPLACE(EnableVertexAttribArray);
	LAYER_REPLACE(DisableVertexAttribArray);
	LAYER_REPLACE(VertexAttribPointer);
	LAYER_REPLACE(VertexAttribIPointer);
	LAYER_REPLACE(VertexAttribI4ui);
	LAYER_REPLACE(DrawElementsBaseVertex);
	real_DrawArrays = halo_glDrawArrays;
	real_DrawElements = halo_glDrawElements;
	LAYER_REPLACE(DrawArrays);
	LAYER_REPLACE(DrawElements);
	LAYER_REPLACE(CreateShader);
	LAYER_REPLACE(ShaderSource);
	LAYER_REPLACE(CompileShader);
	real_GetShaderiv = halo_glGetShaderiv;
	LAYER_REPLACE(GetShaderiv);
	LAYER_REPLACE(AttachShader);
	LAYER_REPLACE(LinkProgram);
	LAYER_REPLACE(UseProgram);
	real_GetUniformLocation = halo_glGetUniformLocation;
	real_Uniform4fv = halo_glUniform4fv;
	real_GetProgramiv = halo_glGetProgramiv;
	real_Uniform1i = halo_glUniform1i;
	real_Uniform1f = halo_glUniform1f;
	real_Uniform2f = halo_glUniform2f;
	LAYER_REPLACE(GetUniformLocation);
	LAYER_REPLACE(Uniform4fv);
	LAYER_REPLACE(GetProgramiv);
	LAYER_REPLACE(Uniform1i);
	LAYER_REPLACE(Uniform1f);
	LAYER_REPLACE(Uniform2f);
	LAYER_REPLACE(GenQueries);
	LAYER_REPLACE(BeginQuery);
	LAYER_REPLACE(EndQuery);
	LAYER_REPLACE(GetQueryObjectuiv);
	/* (ES 2 has no copy between images: the renderer blits, d3d8_gl.c) */
	halo_glCopyImageSubData = NULL;
	halo_glMemoryBarrier = NULL;

	real.GetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attribute_maximum);
	if (attribute_maximum > LAYER_ATTRIBUTES)
		attribute_maximum = LAYER_ATTRIBUTES;
	vertex_arrays[0].used = 1;
	{
		static const unsigned char white[4] = { 255, 255, 255, 255 };

		real_GenTextures(1, &texture_white);
		for (unit = 0; unit < 8; unit++)
		{
			real.ActiveTexture(GL_TEXTURE0 + (GLenum)unit);
			real.BindTexture(GL_TEXTURE_2D, texture_white);
			if (!unit)
			{
				real.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
				real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			}
		}
		real.ActiveTexture(GL_TEXTURE0);
	}
	for (unit = 0; unit < LAYER_TEXTURE_UNITS; unit++)
		unit_samplers[unit] = 0;
	platform_log("gles2: ES 2 filled out to ES 3 for the renderer (%d vertex attributes)", attribute_maximum);
	profile_start();
	bus_error_watch();
	return 1;
}
