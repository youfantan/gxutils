#pragma once

#include <concepts>
#include <iostream>
#include <memory>
#include <cstring>
#include <format>
#include <utility>
#include <mutex>
/*
 *若启用该宏定义，将通过概念约束一些成员模板只能使用数字类型(int8_t/int16_t等)作为模板参数。(C++20及以上可用)
 */
#ifdef BUFFER_STRICT_TEMPLATE_TYPE_CHECK
template<typename T>
concept basic_integral_type = std::is_integral_v<T>;
#endif

class mem_exception : std::exception{
private:
    std::string reason;
public:
    explicit mem_exception(std::string reason) : reason(std::move(reason)) {}
    const char *what() {
        return reason.c_str();
    }
};
/*
 *默认的堆内存分配器
 */
class mem_heap_allocator {
public:
    static void *alloc(size_t size) {
        return malloc(size);
    }
    static void release(void *ptr, size_t size) {
        free(ptr);
    }
};

template<typename Allocator = mem_heap_allocator>
class mem_buffer;

/*
 * 流式访问内存缓冲类。可以通过构造函数构造，也可以通过mem_buffer内建的几个辅助方法直接获取。
 */
template<typename T, typename Buffer = mem_buffer<>>
class mem_stream {
private:
    size_t pos;
    Buffer buffer;
    bool eof_bit {false};
    const size_t step = sizeof(T);
public:
    explicit mem_stream(Buffer& buffer) : pos(0), buffer(buffer) {}
    mem_stream(mem_stream const&) = default;
    bool get(T& t) {
        bool r;
        if ((r = buffer.read(reinterpret_cast<char*>(&t), step, pos))) {
            pos += step;
        }
        if (pos >= buffer.capacity) {
            eof_bit = true;
        }
        return r;
    }

    T get() {
        T t;
        get(t);
        return t;
    }

    T peek() {
        T t = get();
        pos--;
        return t;
    }

    T peek(size_t len) {
        forward(len - 1);
        T t = get();
        back(len);
        return t;
    }

    bool put(T const& t) {
        bool r = buffer.write(reinterpret_cast<const char*>(&t), step, pos);
        pos += step;
        return r;
    }

    void reset() {
        pos = 0;
        eof_bit = false;
    }

    void back() {
        pos -= step;
        if (pos < buffer.capacity) {
            eof_bit = false;
        }
    }

    void back(size_t len) {
        pos -= step * len;
        if (pos < buffer.capacity) {
            eof_bit = false;
        }
    }

    void forward() {
        pos += step;
        if (pos >= buffer.capacity) {
            eof_bit = true;
        }
    }

    void forward(size_t len) {
        pos += step * len;
        if (pos >= buffer.capacity) {
            eof_bit = true;
        }
    }

    T* ptr() {
        return reinterpret_cast<T*>(*buffer.ptr + pos);
    }

    [[nodiscard]] bool eof() const {
        return eof_bit;
    }

    bool eof(size_t s) {
        if (pos + s >= buffer.capacity) {
            return true;
        }
        return false;
    }

    //equals to get()
    mem_stream& operator>>(T& t) {
        get(t);
        return *this;
    }

    //equals to put()
    mem_stream& operator<<(T const& t) {
        put(t);
        return *this;
    }

    explicit mem_stream(mem_stream const&&) = delete;
    mem_stream() = delete;
};

/*
 * mem_buffer(size_t)构造函数将通过capacity从heap内申请一个内存块并将引用计数设为1，任何通过operator=或mem_buffer(mem_buffer const&)引用构造的mem_buffer的每次引用都将使引用技术
 * 增加1，任何mem_buffer的析构函数被调用后会以同步的方式将引用计数减一。
 *
 * 一旦mem_buffer被分配则不能再更改其指向的内容，若需要拷贝mem_buffer请调用拷贝引用构造函数。
 *
 * Allocator是分配器实例，通过以特定Allocator类传入模板参数使用分配器进行内存分配，分配器需实现static void *alloc(size_t)与static void release(void *, size_t)两个函数。默认
 * 以mem_heap_allocator作为默认模板参数。
 *
 * enable_auto_expand若为true，则在调用任何写函数时检查是否越界，若越界则重新分配合适大小的内存，分配大小由字段single_expand_size指定。
 * enable_auto_release若为true，则在引用计数变为0时释放所有动态释放的资源。
 *
 * 成员capacity、mutex均随拷贝引用，在任何一个实例中改变这两个成员字段均会导致所有实例中的两个字段改变。ptr为指向分配内存的二级指针，由Allocator分配后在释放前不再改变，若应用enable_
 * auto_expand则会改变其指向的内存。
 *
 * 以capacity=0构造该类是未定义行为。
 *
 * 该类中的所有函数均为可重入的线程安全函数。
 * */
template<typename Allocator>
class mem_buffer {
    template<typename T, typename Buffer>
    friend class mem_stream;
private:
    size_t& capacity;
    size_t pos;
    size_t single_expand_size {16 * 1024};
    int *ref_counter;
    std::mutex& mutex;
    char **ptr;
    bool enable_auto_release;
    bool enable_auto_expand;
public:
    explicit mem_buffer(size_t capacity, std::mutex *mutex = new std::mutex) : capacity(*new size_t), pos(0), mutex(*mutex), enable_auto_release(true), enable_auto_expand(true) {
        ref_counter = new int[1];
        *ref_counter = 1;
        this->capacity = capacity;
        ptr = reinterpret_cast<char**>(Allocator::alloc(capacity));
        if (ptr == nullptr) {
            throw mem_exception(std::format("cannot allocate memory pointer by allocator {}", typeid(Allocator).name()));
        }
        *ptr = reinterpret_cast<char*>(Allocator::alloc(capacity));
        if (*ptr == nullptr) {
            throw mem_exception(std::format("cannot allocate memory by allocator {}", typeid(Allocator).name()));
        }
        memset(*ptr, 0, capacity);
    }

    mem_buffer(mem_buffer const& buffer) : mutex(buffer.mutex), enable_auto_release(buffer.enable_auto_release), enable_auto_expand(buffer.enable_auto_expand), capacity(buffer.capacity), ref_counter(buffer.ref_counter), ptr(buffer.ptr), pos(buffer.pos) {
        mutex.lock();
#ifdef BUFFER_DEBUG
        std::cout << "called ref copy constructor" << std::endl;
#endif
        ++(*this->ref_counter);
        mutex.unlock();
    }

    bool read(char *dst, size_t const len, size_t const off) const {
        if (len + off > capacity) {
            return false; // EOF
        }
        mutex.lock();
        memcpy(dst, *ptr + off, len);
        mutex.unlock();
        return true;
    }

    bool read(char *dst, size_t const len) {
        bool r = read(dst, len, pos);
        pos += len;
        return r;
    }

    bool write(const char *src, size_t const len, size_t const off) {
        mutex.lock();
        if (len + off > capacity) {
            if (enable_auto_expand) {
                expand();
            } else {
                throw mem_exception("cannot read buffer because its capacity is full");
            }
        }
        memcpy(*ptr + off, src, len);
        mutex.unlock();
        return true;
    }

    bool write(const char *src, const size_t len) {
        bool r = write(src, len, pos);
        pos += len;
        return r;
    }

    size_t auto_expand_size() const {
        return single_expand_size;
    }

    void auto_expand_size(size_t size) {
        single_expand_size = size;
    }

    template<typename T>
#ifdef BUFFER_STRICT_TEMPLATE_TYPE_CHECK
    requires basic_integral_type<T>
#endif
    bool write(T const& t) {
        return write(reinterpret_cast<const char*>(&t), sizeof(T));
    }

    template<typename T>
#ifdef BUFFER_STRICT_TEMPLATE_TYPE_CHECK
    requires basic_integral_type<T>
#endif
bool write(T const&& t) {
        T v = t;
        return write(reinterpret_cast<char*>(&v), sizeof(T));
    }

    template<typename T>
#ifdef BUFFER_STRICT_TEMPLATE_TYPE_CHECK
    requires basic_integral_type<T>
#endif
    bool read(T& t) {
         return read(reinterpret_cast<char*>(&t), sizeof(T));
    }

    template<typename Rt>
#ifdef BUFFER_STRICT_TEMPLATE_TYPE_CHECK
    requires basic_integral_type<T>
#endif
    Rt read() {
        Rt v;
        read(v);
        return v;
    }

    size_t position() const {
        return pos;
    }

    void position(size_t position) {
        this->pos = position;
    }

    void rewind() {
        this->pos = 0;
    }

    bool expand() {
        if (enable_auto_release && enable_auto_expand) {
#ifdef BUFFER_DEBUG
            std::cout << "try expand buffer, new capacity:" << capacity + single_expand_size << std::endl;
#endif
            size_t new_capacity = capacity + single_expand_size;
            char *new_ptr = reinterpret_cast<char*>(Allocator::alloc(new_capacity));
            memset(new_ptr, 0, new_capacity);
            memcpy(new_ptr, *ptr, capacity);
            Allocator::release(*ptr, capacity);
            *ptr = new_ptr;
            capacity = new_capacity;
            return true;
        }
        return false;
    }

    auto get_byte_stream() {
        return mem_stream<uint8_t, mem_buffer>(*this);
    }

    auto get_char_stream() {
        return mem_stream<char, mem_buffer>(*this);
    }

    auto get_char8_stream() {
        return mem_stream<char8_t, mem_buffer>(*this);
    }

    auto get_char16_stream() {
        return mem_stream<char16_t, mem_buffer>(*this);
    }

    auto get_char32_stream() {
        return mem_stream<char32_t, mem_buffer>(*this);
    }

    ~mem_buffer() {
        mutex.lock();
        --(*ref_counter);
#ifdef BUFFER_DEBUG
        std::cout << "ref_counter-1, current: " << *ref_counter << std::endl;
#endif
        mutex.unlock();
        if (*ref_counter == 0 && enable_auto_release) {
#ifdef BUFFER_DEBUG
            std::cout << "buffer released.";
#endif
            Allocator::release(*ptr, capacity);
            Allocator::release(ptr, sizeof(char**));
            delete &mutex;
        } else {

        }
    }

    mem_buffer() = delete;
    mem_buffer const& operator=(mem_buffer const&) = delete;
    mem_buffer const& operator=(mem_buffer const&&) = delete;
    explicit mem_buffer(mem_buffer const&& buffer) = delete;
};

template<typename Allocator = mem_heap_allocator>
using buffer_t = mem_buffer<Allocator>;

template<typename Buffer = mem_buffer<>>
using char_stream = mem_stream<char, Buffer>;

template<typename Buffer = mem_buffer<>>
using char8_stream = mem_stream<char8_t, Buffer>;

template<typename Buffer = mem_buffer<>>
using byte_stream = mem_stream<uint8_t, Buffer>;

template<typename Buffer = mem_buffer<>>
using char16_stream = mem_stream<char16_t, Buffer>;

template<typename Buffer = mem_buffer<>>
using char32_stream = mem_stream<char32_t, Buffer>;