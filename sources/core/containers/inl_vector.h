#pragma once

#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <new>
#include <type_traits>
#include <utility>

namespace tc
{

template <typename T, std::size_t Capacity>
class inl_vector
{
public:
  using value_type = T;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = value_type &;
  using const_reference = const value_type &;
  using pointer = value_type *;
  using const_pointer = const value_type *;
  using iterator = pointer;
  using const_iterator = const_pointer;

  inl_vector() noexcept = default;

  inl_vector(std::initializer_list<value_type> init)
  {
    assert(init.size() <= Capacity && "initializer list exceeds inline vector capacity");
    for (const auto &value : init)
    {
      emplace_back(value);
    }
  }

  inl_vector(const inl_vector &other)
  {
    assign_from(other.begin(), other.end());
  }

  inl_vector(inl_vector &&other) noexcept(std::is_nothrow_move_constructible_v<value_type>)
  {
    move_from(std::move(other));
  }

  inl_vector &operator=(const inl_vector &other)
  {
    if (this != &other)
    {
      clear();
      assign_from(other.begin(), other.end());
    }
    return *this;
  }

  inl_vector &operator=(inl_vector &&other) noexcept(std::is_nothrow_move_constructible_v<value_type> &&
                                                     std::is_nothrow_move_assignable_v<value_type>)
  {
    if (this != &other)
    {
      clear();
      move_from(std::move(other));
    }
    return *this;
  }

  ~inl_vector()
  {
    clear();
  }

  [[nodiscard]] constexpr size_type capacity() const noexcept
  {
    return Capacity;
  }

  void resize(size_type count)
  {
    assert(count <= Capacity && "inl_vector cannot resize beyond its static capacity");

    if (count < m_size)
    {
      for (size_type i = m_size; i > count; --i)
      {
        auto *ptr = object_ptr(i - 1);
        ptr->~value_type();
      }
    }
    else if (count > m_size)
    {
      for (size_type i = m_size; i < count; ++i)
      {
        auto *ptr = storage_ptr(i);
        ::new (static_cast<void *>(ptr)) value_type();
      }
    }

    m_size = count;
  }

  [[nodiscard]] size_type size() const noexcept
  {
    return m_size;
  }

  [[nodiscard]] bool empty() const noexcept
  {
    return m_size == 0;
  }

  reference operator[](size_type index) noexcept
  {
    assert(index < m_size);
    return *object_ptr(index);
  }

  const_reference operator[](size_type index) const noexcept
  {
    assert(index < m_size);
    return *object_ptr(index);
  }

  reference front() noexcept
  {
    assert(!empty());
    return *object_ptr(0);
  }

  const_reference front() const noexcept
  {
    assert(!empty());
    return *object_ptr(0);
  }

  reference back() noexcept
  {
    assert(!empty());
    return *object_ptr(m_size - 1);
  }

  const_reference back() const noexcept
  {
    assert(!empty());
    return *object_ptr(m_size - 1);
  }

  pointer data() noexcept
  {
    if constexpr (Capacity == 0)
    {
      return nullptr;
    }
    return storage_ptr(0);
  }

  const_pointer data() const noexcept
  {
    if constexpr (Capacity == 0)
    {
      return nullptr;
    }
    return storage_ptr(0);
  }

  iterator begin() noexcept
  {
    return data();
  }

  const_iterator begin() const noexcept
  {
    return data();
  }

  const_iterator cbegin() const noexcept
  {
    return data();
  }

  iterator end() noexcept
  {
    if constexpr (Capacity == 0)
    {
      return nullptr;
    }
    return storage_ptr(0) + m_size;
  }

  const_iterator end() const noexcept
  {
    if constexpr (Capacity == 0)
    {
      return nullptr;
    }
    return storage_ptr(0) + m_size;
  }

  const_iterator cend() const noexcept
  {
    if constexpr (Capacity == 0)
    {
      return nullptr;
    }
    return storage_ptr(0) + m_size;
  }

  template <typename... Args>
  reference emplace_back(Args &&...args)
  {
    assert(m_size < Capacity && "inl_vector capacity exceeded");
    auto *ptr = storage_ptr(m_size);
    ::new (static_cast<void *>(ptr)) value_type(std::forward<Args>(args)...);
    ++m_size;
    return *ptr;
  }

  void push_back(const value_type &value)
  {
    emplace_back(value);
  }

  void push_back(value_type &&value)
  {
    emplace_back(std::move(value));
  }

  void pop_back()
  {
    assert(!empty());
    auto *ptr = object_ptr(m_size - 1);
    ptr->~value_type();
    --m_size;
  }

  void clear() noexcept
  {
    for (size_type i = m_size; i > 0; --i)
    {
      auto *ptr = object_ptr(i - 1);
      ptr->~value_type();
    }
    m_size = 0;
  }

  void clear_fast() noexcept
  {
    m_size = 0;
  }

private:
  pointer storage_ptr(size_type index) noexcept
  {
    if constexpr (Capacity == 0)
    {
      return nullptr;
    }
    assert(index < Capacity);
    return reinterpret_cast<pointer>(&m_storage[index]);
  }

  const_pointer storage_ptr(size_type index) const noexcept
  {
    if constexpr (Capacity == 0)
    {
      return nullptr;
    }
    assert(index < Capacity);
    return reinterpret_cast<const_pointer>(&m_storage[index]);
  }

  pointer object_ptr(size_type index) noexcept
  {
    return std::launder(storage_ptr(index));
  }

  const_pointer object_ptr(size_type index) const noexcept
  {
    return std::launder(storage_ptr(index));
  }

  template <typename InputIt>
  void assign_from(InputIt first, InputIt last)
  {
    for (; first != last; ++first)
    {
      emplace_back(*first);
    }
  }

  void move_from(inl_vector &&other)
  {
    for (size_type i = 0; i < other.m_size; ++i)
    {
      emplace_back(std::move(other[i]));
    }
    other.clear();
  }

  using storage_type = typename std::aligned_storage<sizeof(value_type), alignof(value_type)>::type;

  storage_type m_storage[Capacity > 0 ? Capacity : 1];
  size_type m_size = 0;
};

} // namespace tc

