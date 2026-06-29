#pragma once
#include <cstddef>
#include <utility>

namespace Gfx::DeckLink
{

/// Minimal RAII COM smart pointer (mingw-w64 has no Microsoft::WRL::ComPtr).
/// `put()` is for COM out-params that return an already-AddRef'd interface
/// (IDeckLinkIterator::Next, QueryInterface, CreateVideoFrame, ...).
template <class T>
class ComPtr
{
public:
  ComPtr() = default;
  ComPtr(std::nullptr_t) noexcept {}
  explicit ComPtr(T* p, bool addRef = false) noexcept : m_p{p}
  {
    if(m_p && addRef)
      m_p->AddRef();
  }
  ComPtr(const ComPtr& o) noexcept : m_p{o.m_p}
  {
    if(m_p)
      m_p->AddRef();
  }
  ComPtr(ComPtr&& o) noexcept : m_p{o.m_p} { o.m_p = nullptr; }
  ComPtr& operator=(const ComPtr& o) noexcept
  {
    if(this != &o)
    {
      reset();
      m_p = o.m_p;
      if(m_p)
        m_p->AddRef();
    }
    return *this;
  }
  ComPtr& operator=(ComPtr&& o) noexcept
  {
    if(this != &o)
    {
      reset();
      m_p = o.m_p;
      o.m_p = nullptr;
    }
    return *this;
  }
  ~ComPtr() { reset(); }

  void reset() noexcept
  {
    if(m_p)
    {
      m_p->Release();
      m_p = nullptr;
    }
  }
  T* get() const noexcept { return m_p; }
  T* operator->() const noexcept { return m_p; }
  explicit operator bool() const noexcept { return m_p != nullptr; }

  /// Out-param slot (releases any current ref first). The callee hands back an
  /// owned (AddRef'd) interface, which we adopt without an extra AddRef.
  T** put() noexcept
  {
    reset();
    return &m_p;
  }
  /// Out-param slot typed as void** (QueryInterface / CoCreateInstance).
  void** putVoid() noexcept { return reinterpret_cast<void**>(put()); }

  T* release() noexcept
  {
    T* t = m_p;
    m_p = nullptr;
    return t;
  }

private:
  T* m_p{};
};

} // namespace Gfx::DeckLink
