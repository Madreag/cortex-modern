#pragma once

#include <atomic>

namespace RTE {
class MovableObject;

// Non-owning native links expire when their target is reset or destroyed.
// Each link belongs to its target's intrusive list; no world scan is needed.
class MovableObjectReference {
public:
    MovableObjectReference(const MovableObject* object = nullptr);
    MovableObjectReference(const MovableObjectReference& reference);
    MovableObjectReference(MovableObjectReference&& reference) noexcept;
    ~MovableObjectReference();
    MovableObjectReference& operator=(const MovableObject* object);
    MovableObjectReference& operator=(const MovableObjectReference& reference);
    MovableObjectReference& operator=(MovableObjectReference&& reference) noexcept;

    const MovableObject* get() const noexcept { return m_Object.load(std::memory_order_acquire); }
    operator const MovableObject*() const noexcept { return get(); }
    const MovableObject* operator->() const noexcept { return get(); }

private:
    friend class MovableObject;
    std::atomic<const MovableObject*> m_Object{nullptr};
    MovableObjectReference* m_Previous = nullptr;
    MovableObjectReference* m_Next = nullptr;
    long* m_ExpiryIdentity = nullptr;

    void Detach();
    void AttachLocked(const MovableObject* object);
    void Copy(const MovableObjectReference& reference);
    static void Expire(const MovableObject* object);
};
}
