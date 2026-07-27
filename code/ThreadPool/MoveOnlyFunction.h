#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

template <class Signature>
class MoveOnlyFunction;

template <>
class MoveOnlyFunction<void()>
{
public:
    MoveOnlyFunction() noexcept
        : m_ptr(nullptr)
        , m_local(false)
    {
    }

    MoveOnlyFunction(std::nullptr_t) noexcept
        : MoveOnlyFunction()
    {
    }

    template <class F,
              class = typename std::enable_if<
                  !std::is_same<typename std::decay<F>::type, MoveOnlyFunction>::value>::type>
    MoveOnlyFunction(F&& f)
        : m_ptr(nullptr)
        , m_local(false)
    {
        using Decayed = typename std::decay<F>::type;
        using Impl = CallableImpl<Decayed>;
        if (sizeof(Impl) <= BufferSize
            && alignof(Impl) <= alignof(std::max_align_t)
            && std::is_nothrow_move_constructible<Decayed>::value)
        {
            m_ptr = ::new (static_cast<void*>(m_buffer)) Impl(std::forward<F>(f));
            m_local = true;
        }
        else
        {
            m_ptr = new Impl(std::forward<F>(f));
            m_local = false;
        }
    }

    MoveOnlyFunction(MoveOnlyFunction&& other) noexcept
        : m_ptr(nullptr)
        , m_local(false)
    {
        MoveFrom(std::move(other));
    }

    MoveOnlyFunction& operator=(MoveOnlyFunction&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    MoveOnlyFunction(const MoveOnlyFunction&) = delete;
    MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

    ~MoveOnlyFunction()
    {
        Reset();
    }

    explicit operator bool() const noexcept
    {
        return m_ptr != nullptr;
    }

    void operator()()
    {
        m_ptr->Invoke();
    }

private:
    static constexpr std::size_t BufferSize = 64;

    struct CallableBase
    {
        virtual ~CallableBase() = default;
        virtual void Invoke() = 0;
        virtual void RelocateTo(void* buffer) noexcept = 0;
    };

    template <class F>
    struct CallableImpl : CallableBase
    {
        F m_f;

        template <class U>
        explicit CallableImpl(U&& u)
            : m_f(std::forward<U>(u))
        {
        }

        void Invoke() override
        {
            m_f();
        }

        void RelocateTo(void* buffer) noexcept override
        {
            ::new (buffer) CallableImpl(std::move(m_f));
        }
    };

    alignas(std::max_align_t) unsigned char m_buffer[BufferSize];
    CallableBase* m_ptr;
    bool m_local;

    void Reset() noexcept
    {
        if (m_ptr == nullptr)
        {
            return;
        }
        if (m_local)
        {
            m_ptr->~CallableBase();
        }
        else
        {
            delete m_ptr;
        }
        m_ptr = nullptr;
        m_local = false;
    }

    void MoveFrom(MoveOnlyFunction&& other) noexcept
    {
        if (other.m_ptr == nullptr)
        {
            return;
        }
        if (other.m_local)
        {
            other.m_ptr->RelocateTo(m_buffer);
            m_ptr = reinterpret_cast<CallableBase*>(m_buffer);
            m_local = true;
            other.m_ptr->~CallableBase();
            other.m_ptr = nullptr;
            other.m_local = false;
        }
        else
        {
            m_ptr = other.m_ptr;
            m_local = false;
            other.m_ptr = nullptr;
        }
    }
};
