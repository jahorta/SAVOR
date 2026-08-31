#pragma once

#include <optional>
#include <utility>

namespace savorqt::gui {

enum class DraftReplacementReason {
    InitialLoad,
    ExplicitReset,
    UserRequestedEntityChange,
    SaveSucceeded,
    BackingEntityRemoved,
};

template <typename T>
class DraftState
{
public:
    const T& value() const { return value_; }
    const T& baseline() const { return baseline_; }
    bool dirty() const { return dirty_; }
    bool backingStale() const { return backing_stale_; }
    bool backingMissing() const { return backing_missing_; }

    void replace(T value, DraftReplacementReason)
    {
        value_ = std::move(value);
        baseline_ = value_;
        dirty_ = false;
        backing_stale_ = false;
        backing_missing_ = false;
    }

    void edit(T value)
    {
        value_ = std::move(value);
        dirty_ = !(value_ == baseline_);
    }

    void commit()
    {
        baseline_ = value_;
        dirty_ = false;
        backing_stale_ = false;
        backing_missing_ = false;
    }

    void observeBacking(const T& value)
    {
        if (!dirty_) {
            replace(value, DraftReplacementReason::InitialLoad);
        } else if (!(value == baseline_)) {
            backing_stale_ = true;
        }
    }

    void markBackingMissing() { backing_missing_ = true; }

private:
    T value_{};
    T baseline_{};
    bool dirty_ = false;
    bool backing_stale_ = false;
    bool backing_missing_ = false;
};

template <typename T>
class AppliedQuery
{
public:
    const T& value() const { return value_; }
    void apply(T value) { value_ = std::move(value); }

private:
    T value_{};
};

template <typename Key, typename T>
struct EntityDraft {
    std::optional<Key> key;
    DraftState<T> draft;

    bool isBoundTo(const Key& candidate) const { return key.has_value() && *key == candidate; }
    void bind(Key new_key, T value, DraftReplacementReason reason)
    {
        key = std::move(new_key);
        draft.replace(std::move(value), reason);
    }
};

template <typename Key>
class KeyedSelection
{
public:
    const std::optional<Key>& key() const { return key_; }
    void select(Key key) { key_ = std::move(key); }
    void clear() { key_.reset(); }
    bool is(const Key& key) const { return key_.has_value() && *key_ == key; }

private:
    std::optional<Key> key_;
};

template <typename Key, typename Draft>
struct EditorSession {
    EntityDraft<Key, Draft> entity;
    bool active = false;

    void close()
    {
        active = false;
        entity.key.reset();
    }
};

} // namespace savorqt::gui
