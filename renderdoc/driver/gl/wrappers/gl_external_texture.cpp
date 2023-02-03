#include "../gl_driver.h"

rdcarray<byte> WrappedOpenGL::ReadExternalTextureData(GLuint texture)
{
  rdcarray<byte> pixels;
  GLuint curr_tex_ext = 0; // for any current texture unit
  GL.glGetIntegerv(eGL_TEXTURE_BINDING_EXTERNAL_OES, (GLint *)&curr_tex_ext);
  GL.glBindTexture(eGL_TEXTURE_EXTERNAL_OES, texture);

  GLint width = 0, height = 0;
  GLenum internal_format = eGL_NONE, format = eGL_NONE;
  GL.glGetTexLevelParameteriv(eGL_TEXTURE_EXTERNAL_OES, 0, eGL_TEXTURE_WIDTH, &width);
  GL.glGetTexLevelParameteriv(eGL_TEXTURE_EXTERNAL_OES, 0, eGL_TEXTURE_HEIGHT, &height);
  GL.glGetTexLevelParameteriv(eGL_TEXTURE_EXTERNAL_OES, 0, eGL_TEXTURE_INTERNAL_FORMAT,
                              (GLint *)&internal_format);
  size_t size = (size_t)width * (size_t)height;

  switch(internal_format)
  {
    case eGL_R8:
      format = eGL_RED;
      break;
    case eGL_RG8:
      format = eGL_RG;
      size *= 2;
      break;
    case eGL_RGB8:
      format = eGL_RGB;
      size *= 3;
      break;
    case eGL_RGBA8:
      format = eGL_RGBA;
      size *= 4;
      break;
    default: RDCERR("Unknown internal format 0x%X", internal_format); return false;
  }

  pixels.resize(size);
  // 
  // read pixels. ref: https://developer.arm.com/documentation/ka004859/1-0
  GLuint curr_read_fb = 0, curr_pixpackbuf = 0, fb = 0;
  GL.glGetIntegerv(eGL_PIXEL_PACK_BUFFER_BINDING, (GLint *)&curr_pixpackbuf);
  GL.glBindFramebuffer(eGL_PIXEL_PACK_BUFFER, 0);
  GL.glGenFramebuffers(1, &fb);
  GL.glGetIntegerv(eGL_READ_FRAMEBUFFER_BINDING, (GLint *)&curr_read_fb);
  GL.glBindFramebuffer(eGL_READ_FRAMEBUFFER, fb);
  GL.glFramebufferTexture2D(eGL_READ_FRAMEBUFFER, eGL_COLOR_ATTACHMENT0, eGL_TEXTURE_EXTERNAL_OES,
                            texture, 0);
  GLint curr_pixpackalign = 0;
  GL.glGetIntegerv(eGL_PACK_ALIGNMENT, &curr_pixpackalign);
  GL.glPixelStorei(eGL_PACK_ALIGNMENT, 1);
  GL.glReadPixels(0, 0, width, height, format, eGL_UNSIGNED_BYTE, pixels.data());
  GL.glFinish();
  GL.glPixelStorei(eGL_PACK_ALIGNMENT, curr_pixpackalign);
  GL.glBindFramebuffer(eGL_READ_FRAMEBUFFER, curr_read_fb);
  GL.glDeleteFramebuffers(1, &fb);
  GL.glBindBuffer(eGL_PIXEL_PACK_BUFFER, curr_pixpackbuf);

  RDCASSERT(GL.glGetError() == GL_NO_ERROR);

  return pixels;
}

#include "../egl_dispatch_table.h"

#if defined(RENDERDOC_PLATFORM_ANDROID)
static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC eglGetNativeClientBufferANDROID = nullptr;
#endif
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = nullptr;


EGLImageKHR WrappedOpenGL::CreateEGLImage(GLint width, GLint height, GLenum internal_format)
{
  EGLImageKHR image = EGL_NO_IMAGE_KHR;

  if(!eglCreateImageKHR)
  {
#if defined(RENDERDOC_PLATFORM_ANDROID)
      eglGetNativeClientBufferANDROID = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress(
          "eglGetNativeClientBufferANDROID");
      RDCASSERT(eglGetNativeClientBufferANDROID);
#endif
      eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
      RDCASSERT(eglCreateImageKHR);
      eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
      RDCASSERT(eglDestroyImageKHR);
  }

#if defined(RENDERDOC_PLATFORM_ANDROID)
  uint32_t buffer_format = 0;
  switch(internal_format)
  {
    //case eGL_R8: buffer_format = AHARDWAREBUFFER_FORMAT_R8_UNORM;
    //  break;
    case eGL_RGB8:
      buffer_format = AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM;
      break;
    case eGL_RGBA8:
      buffer_format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
      break;
    default: RDCERR("Unknown or unsupported internal format 0x%X", internal_format);
  }
  AHardwareBuffer *hardware_buffer = nullptr;
  EGLClientBuffer client_buffer = nullptr;

  AHardwareBuffer_Desc buffer_desc{};
  buffer_desc.width  = width;
  buffer_desc.height = height;
  buffer_desc.format = buffer_format;
  buffer_desc.layers = 1;
  buffer_desc.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

  int res = AHardwareBuffer_allocate(&buffer_desc, &hardware_buffer);
  RDCASSERT(res == 0);
  client_buffer = eglGetNativeClientBufferANDROID(hardware_buffer);
  RDCASSERT(client_buffer);
  image = eglCreateImageKHR(EGL.GetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                client_buffer, nullptr);
  RDCASSERT(image != EGL_NO_IMAGE_KHR);

  ExternalTextureResources etr;
  etr.hw_buffer = hardware_buffer;
  etr.cl_buffer = client_buffer;
  etr.image = image;
  m_ExternalTextureResources.push_back(etr);

#endif // if defined(RENDERDOC_PLATFORM_ANDROID)
  return image;
}

void WrappedOpenGL::ReleaseExternalTextureResources()
{
  for (auto& etr : m_ExternalTextureResources)
  {
    if (etr.image != EGL_NO_IMAGE_KHR)
      eglDestroyImageKHR(eglGetCurrentDisplay(), etr.image);
#if defined(RENDERDOC_PLATFORM_ANDROID)
    if (etr.hw_buffer)
      AHardwareBuffer_release(etr.hw_buffer);
#endif
  }
  m_ExternalTextureResources.clear();
}

void WrappedOpenGL::WriteExternalTexture(EGLImageKHR egl_image, const byte *pixels, uint64_t size)
{
#if defined(RENDERDOC_PLATFORM_ANDROID)
  ExternalTextureResources *texdata = nullptr;
  for(size_t i = 0; i < m_ExternalTextureResources.size(); i++)
  {
    if(m_ExternalTextureResources[i].image == egl_image)
    {
      texdata = &m_ExternalTextureResources[i];
      break;
    }  
  }
  if(!texdata)
  {
    return;
  }
  
  RDCASSERT(texdata->hw_buffer);
  AHardwareBuffer_Desc hw_buf_desc {};
  AHardwareBuffer_describe(texdata->hw_buffer, &hw_buf_desc);
  byte *pwrite = nullptr;
  int res = AHardwareBuffer_lock(texdata->hw_buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1,
                                 nullptr, (void **)&pwrite);
  RDCASSERT(res == 0);
  if(hw_buf_desc.stride == hw_buf_desc.width)
  {
    memcpy(pwrite, pixels, size); // copy at once
  }
  else    // copy row by row
  {
    uint32_t pixel_size;
    switch(hw_buf_desc.format)
    {
      //case AHARDWAREBUFFER_FORMAT_R8_UNORM: pixel_size = 1; break;
      case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM: pixel_size = 3; break;
      case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
      case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM: pixel_size = 4; break;
      default:
        pixel_size = 0;
        hw_buf_desc.height = 0;    // to prevent copying
        RDCERR("Unknown or unsupported hardware buffer format 0x%X", hw_buf_desc.format);
    }
    hw_buf_desc.width *= pixel_size;     // hw_buf_desc.width = src row size in bytes
    hw_buf_desc.stride *= pixel_size;    // hw_buf_desc.stride = dst row size in bytes
    for(uint32_t h = 0; h < hw_buf_desc.height; ++h)
    {
      memcpy(pwrite, pixels, hw_buf_desc.width);
      pixels += hw_buf_desc.width;
      pwrite += hw_buf_desc.stride;
    }
  }
  res = AHardwareBuffer_unlock(texdata->hw_buffer, nullptr);
  RDCASSERT(res == 0);
#endif // if defined(RENDERDOC_PLATFORM_ANDROID)
}
