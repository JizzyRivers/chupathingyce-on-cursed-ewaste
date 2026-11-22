/*
ANDROID32_GL.C

The renderer's few OpenGL ES calls that are not one function (the names are
declared in port/linux/src/xgpu.h), as upstream's Android build has them
(port/android/host/host_gl.c, which is tied to its guest image): an
extension test, a buffer read-back for the visibility tests' counters, the
ring of fences that keeps a frame's streamed vertices from being written
while the GPU still reads them, and a buffer write that does not discard.
Built with the host's ABI (bionic and the NDK's ES 3.2 headers only).
*/

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <stdint.h>
#include <string.h>

int host_gl_has_extension(const char *name)
{
	GLint count = 0, index;

	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *extension = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)index);

		if (extension && !strcmp(extension, name))
			return 1;
	}
	return 0;
}

void host_gl_read_buffer(unsigned int buffer, unsigned int offset, unsigned int size, void *data)
{
	const void *mapping;

	glBindBuffer(GL_COPY_READ_BUFFER, buffer);
	mapping = glMapBufferRange(GL_COPY_READ_BUFFER, offset, size, GL_MAP_READ_BIT);
	if (mapping)
	{
		memcpy(data, mapping, size);
		glUnmapBuffer(GL_COPY_READ_BUFFER);
	}
	else
	{
		memset(data, 0, size);
	}
	glBindBuffer(GL_COPY_READ_BUFFER, 0);
}

#define FRAME_FENCE_SLOTS 8

static GLsync frame_fences[FRAME_FENCE_SLOTS];

void host_gl_fence_frame(unsigned int slot)
{
	if (slot >= FRAME_FENCE_SLOTS)
		return;
	if (frame_fences[slot])
		glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void host_gl_wait_frame(unsigned int slot)
{
	if (slot >= FRAME_FENCE_SLOTS || !frame_fences[slot])
		return;
	/* at most a second: a lost context must not hang the game */
	glClientWaitSync(frame_fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
	glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = NULL;
}

/* the range is written without the driver discarding its old contents
(upstream measured the discard at most of a slow frame on Adreno): the
renderer only writes ranges no queued draw reads, host_gl_wait_frame having
released the ring slot first */
void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data)
{
	void *mapping = glMapBufferRange(target, offset, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);

	if (!mapping)
	{
		glBufferSubData(target, offset, size, data);
		return;
	}
	memcpy(mapping, data, size);
	glUnmapBuffer(target);
}
