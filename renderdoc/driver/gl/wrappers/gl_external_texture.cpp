#include "../gl_driver.h"
#include "../egl_dispatch_table.h"

rdcarray<byte> WrappedOpenGL::ReadExternalTextureData(GLuint texture)
{
  rdcarray<byte> pixels;
  GLuint prevTex = 0; // for any current texture unit
  GL.glGetIntegerv(eGL_TEXTURE_BINDING_EXTERNAL_OES, (GLint *)&prevTex);
  GL.glBindTexture(eGL_TEXTURE_EXTERNAL_OES, texture);

  GLint width = 0, height = 0;
  GLenum internalFormat = eGL_NONE;
  GL.glGetTexLevelParameteriv(eGL_TEXTURE_EXTERNAL_OES, 0, eGL_TEXTURE_WIDTH, &width);
  GL.glGetTexLevelParameteriv(eGL_TEXTURE_EXTERNAL_OES, 0, eGL_TEXTURE_HEIGHT, &height);
  GL.glGetTexLevelParameteriv(eGL_TEXTURE_EXTERNAL_OES, 0, eGL_TEXTURE_INTERNAL_FORMAT,
                              (GLint *)&internalFormat);
  GL.glBindTexture(eGL_TEXTURE_EXTERNAL_OES, prevTex);

  size_t size = GetByteSize(width, height, 1, GetBaseFormat(internalFormat), eGL_UNSIGNED_BYTE);

  pixels.resize(size);
  // 
  // read pixels. ref: https://developer.arm.com/documentation/ka004859/1-0
  GLuint prevReadFramebuffer = 0, prevPixelPackBuffer = 0, fb = 0;
  GL.glGetIntegerv(eGL_PIXEL_PACK_BUFFER_BINDING, (GLint *)&prevPixelPackBuffer);
  GL.glBindBuffer(eGL_PIXEL_PACK_BUFFER, 0);
  GL.glGenFramebuffers(1, &fb);
  GL.glGetIntegerv(eGL_READ_FRAMEBUFFER_BINDING, (GLint *)&prevReadFramebuffer);
  GL.glBindFramebuffer(eGL_READ_FRAMEBUFFER, fb);
  GL.glFramebufferTexture2D(eGL_READ_FRAMEBUFFER, eGL_COLOR_ATTACHMENT0, eGL_TEXTURE_EXTERNAL_OES,
                            texture, 0);

  GLint prevPixelPackAlign = 0;
  GL.glGetIntegerv(eGL_PACK_ALIGNMENT, &prevPixelPackAlign);
  GL.glPixelStorei(eGL_PACK_ALIGNMENT, 1);
  GL.glReadPixels(0, 0, width, height, GetBaseFormat(internalFormat), eGL_UNSIGNED_BYTE, pixels.data());
  GL.glFinish();
  GL.glPixelStorei(eGL_PACK_ALIGNMENT, prevPixelPackAlign);
  GL.glBindFramebuffer(eGL_READ_FRAMEBUFFER, prevReadFramebuffer);
  GL.glDeleteFramebuffers(1, &fb);
  GL.glBindBuffer(eGL_PIXEL_PACK_BUFFER, prevPixelPackBuffer);

  return pixels;
}

EGLImageKHR WrappedOpenGL::CreateEGLImage(GLint width, GLint height, GLenum internal_format)
{
  EGLImageKHR image = EGL_NO_IMAGE_KHR;

#if defined(RENDERDOC_PLATFORM_ANDROID)
  uint32_t buffer_format = 0;
  switch(internal_format)
  {
    case eGL_RGB8:
      buffer_format = AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM;
      break;
    case eGL_RGBA8:
      buffer_format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
      break;
    default: RDCERR("Unsupported internal format 0x%X", internal_format);
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
  client_buffer = EGL.GetNativeClientBufferANDROID(hardware_buffer);
  RDCASSERT(client_buffer);
  image = EGL.CreateImageKHR(EGL.GetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
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
    if(etr.image != EGL_NO_IMAGE_KHR)
    {
      if(EGL.DestroyImageKHR)
      {
        EGL.DestroyImageKHR(eglGetCurrentDisplay(), etr.image);
      }
    }
#if defined(RENDERDOC_PLATFORM_ANDROID)
    if(etr.hw_buffer)
    {
      AHardwareBuffer_release(etr.hw_buffer);
    }
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
  else // copy row by row
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
