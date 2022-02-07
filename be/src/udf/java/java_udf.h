// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#pragma once
#include <memory>

#include "common/status.h"
#include "jni.h"
#include "runtime/primitive_type.h"

// implements by libhdfs
// hadoop-hdfs-native-client/src/main/native/libhdfs/jni_helper.c
// Why do we need to use this function?
// 1. a thread can not attach to more than one virtual machine
// 2. libhdfs depends on this function and does some initialization,
// if the JVM has already created it, it won't create it anymore.
// If we skip this function call will cause libhdfs to miss some initialization operations
extern "C" JNIEnv* getJNIEnv(void);

#define DEFINE_JAVA_PRIM_TYPE(TYPE) \
    jclass _class_##TYPE;           \
    jmethodID _value_of_##TYPE;     \
    jmethodID _val_##TYPE;

#define DECLARE_NEW_BOX(TYPE, CLAZZ) \
    jobject new##CLAZZ(TYPE value);  \
    TYPE val##TYPE(jobject obj);

namespace starrocks::vectorized {

// Restrictions on use:
// can only be used in pthread, not in bthread
// thread local helper
class JVMFunctionHelper {
public:
    static JVMFunctionHelper& getInstance();
    JVMFunctionHelper(const JVMFunctionHelper&) = delete;
    // get env
    JNIEnv* getEnv() { return _env; }
    // Arrays.toString()
    std::string array_to_string(jobject object);
    // Object::toString()
    std::string to_string(jobject obj);
    std::string to_cxx_string(jstring str);
    std::string dumpExceptionString(jthrowable throwable);
    jmethodID getToStringMethod(jclass clazz);
    jstring to_jstring(const std::string& str);
    jmethodID getMethod(jclass clazz, const std::string& method, const std::string& sig);
    jmethodID getStaticMethod(jclass clazz, const std::string& method, const std::string& sig);

    DECLARE_NEW_BOX(uint8_t, Boolean)
    DECLARE_NEW_BOX(int8_t, Byte)
    DECLARE_NEW_BOX(int16_t, Short)
    DECLARE_NEW_BOX(int32_t, Integer)
    DECLARE_NEW_BOX(int64_t, Long)
    DECLARE_NEW_BOX(float, Float)
    DECLARE_NEW_BOX(double, Double)

    jobject newString(const char* data, size_t size);

    Slice sliceVal(jstring jstr);
    size_t string_length(jstring jstr);
    Slice sliceVal(jstring jstr, std::string* buffer);
    // replace '.' as '/'
    // eg: java.lang.Integer -> java/lang/Integer
    static std::string to_jni_class_name(const std::string& name);

private:
    JVMFunctionHelper(JNIEnv* env) : _env(env) {}
    void _init();
    void _add_class_path(const std::string& path);

private:
    JNIEnv* _env;

    DEFINE_JAVA_PRIM_TYPE(boolean);
    DEFINE_JAVA_PRIM_TYPE(byte);
    DEFINE_JAVA_PRIM_TYPE(short);
    DEFINE_JAVA_PRIM_TYPE(int);
    DEFINE_JAVA_PRIM_TYPE(long);
    DEFINE_JAVA_PRIM_TYPE(float);
    DEFINE_JAVA_PRIM_TYPE(double);

    jclass _object_class;
    jclass _string_class;
    jclass _throwable_class;
    jclass _jarrays_class;

    jmethodID _string_construct_with_bytes;

    jobject _utf8_charsets;
};

// Used for UDAF serialization and deserialization,
// providing a C++ memory space for Java to access.
// DirectByteBuffer does not hold ownership of this memory space
// Handle will be freed during destructuring,
// but no operations will be done on this memory space
class DirectByteBuffer {
public:
    static constexpr const char* JNI_CLASS_NAME = "java/nio/ByteBuffer";

    DirectByteBuffer(void* data, int capacity);
    DirectByteBuffer(jobject&& handle, void* data, int capacity)
            : _handle(std::move(handle)), _data(data), _capacity(capacity) {}
    ~DirectByteBuffer();

    DirectByteBuffer(const DirectByteBuffer&) = delete;
    DirectByteBuffer& operator=(const DirectByteBuffer& other) = delete;

    DirectByteBuffer(DirectByteBuffer&& other) {
        _handle = other._handle;
        _data = other._data;
        _capacity = other._capacity;

        other._handle = nullptr;
        other._data = nullptr;
        other._capacity = 0;
    }

    DirectByteBuffer& operator=(DirectByteBuffer&& other) {
        DirectByteBuffer tmp(std::move(other));
        std::swap(this->_handle, tmp._handle);
        std::swap(this->_data, tmp._data);
        std::swap(this->_capacity, tmp._capacity);
        return *this;
    }
    // thread safe
    void clear();
    jobject handle() { return _handle; }
    void* data() { return _data; }
    int capacity() { return _capacity; }

private:
    jobject _handle;
    void* _data;
    int _capacity;
};

// A Class object created from the ClassLoader that can be accessed by multiple threads
class JVMClass {
public:
    JVMClass(jobject&& clazz) : _clazz(std::move(clazz)) {}
    ~JVMClass();
    JVMClass(const JVMClass&) = delete;

    JVMClass& operator=(const JVMClass&&) = delete;
    JVMClass& operator=(const JVMClass& other) = delete;

    JVMClass(JVMClass&& other) {
        _clazz = other._clazz;
        other._clazz = nullptr;
    }

    JVMClass& operator=(JVMClass&& other) {
        JVMClass tmp(std::move(other));
        std::swap(this->_clazz, tmp._clazz);
        return *this;
    }

    jclass clazz() const { return (jclass)_clazz; }

    // Create a new instance using the default constructor
    Status newInstance(jobject* object) const;

private:
    jobject _clazz;
};

// For loading UDF Class
// Not thread safe
class ClassLoader {
public:
    // Handle
    ClassLoader(std::string path) : _path(std::move(path)) {}
    ~ClassLoader();

    ClassLoader& operator=(const ClassLoader& other) = delete;
    ClassLoader(const ClassLoader&) = delete;
    // get class
    JVMClass getClass(const std::string& className);
    Status init();

private:
    std::string _path;
    jmethodID _get_class = nullptr;
    jobject _handle = nullptr;
};

struct MethodTypeDescriptor {
    PrimitiveType type;
    bool is_box;
    bool is_array;
};

struct JavaMethodDescriptor {
    std::string sign; // sign
    std::string name; // function name
    std::vector<MethodTypeDescriptor> method_desc;
    // thread safe
    jmethodID get_method_id(jclass clazz) const;
};

// Used to get function signatures
class ClassAnalyzer {
public:
    ClassAnalyzer() = default;
    ~ClassAnalyzer() = default;
    Status has_method(jclass clazz, const std::string& method, bool* has);
    Status get_signature(jclass clazz, const std::string& method, std::string* sign);
    Status get_method_desc(const std::string& sign, std::vector<MethodTypeDescriptor>* desc);
    Status get_udaf_method_desc(const std::string& sign, std::vector<MethodTypeDescriptor>* desc);
};

class UDFHelper {
public:
    jobject create_boxed_array(int type, int num_rows, bool nullable, DirectByteBuffer* buffer, int sz);
};

struct JavaUDFContext {
    JavaUDFContext() = default;
    ~JavaUDFContext();

    std::unique_ptr<ClassLoader> udf_classloader;
    std::unique_ptr<ClassAnalyzer> analyzer;
    JVMClass udf_class = nullptr;
    jobject udf_handle;

    // Java Method
    std::unique_ptr<JavaMethodDescriptor> prepare;
    std::unique_ptr<JavaMethodDescriptor> evaluate;
    std::unique_ptr<JavaMethodDescriptor> close;
};

// Function
struct JavaUDAFContext;

class UDAFFunction {
public:
    UDAFFunction(jobject udaf_state_clazz, jobject udaf_clazz, jobject udaf_handle, JavaUDAFContext* ctx)
            : _udaf_state_clazz(udaf_state_clazz), _udaf_clazz(udaf_clazz), _udaf_handle(udaf_handle), _ctx(ctx) {}
    // create a new state for UDAF
    jobject create();
    // destroy state
    void destroy(jobject state);
    // UDAF Update Function
    void update(jvalue* val);
    // UDAF merge
    void merge(jobject state, jobject buffer);
    void serialize(jobject state, jobject buffer);
    // UDAF State serialize_size
    int serialize_size(jobject state);
    // UDAF finalize
    jvalue finalize(jobject state);

    // WindowFunction reset
    void reset(jobject state);
    // WindowFunction getValues
    jobject get_values(jobject state, int start, int end);
    // WindowFunction updateBatch
    jobject window_update_batch(jobject state, int64_t peer_group_start, int64_t peer_group_end, int64_t frame_start,
                                int64_t frame_end, int col_sz, jobject* cols);

private:
    jobject _udaf_state_clazz;
    jobject _udaf_clazz;
    jobject _udaf_handle;
    JavaUDAFContext* _ctx;
};

struct JavaUDAFContext {
    std::unique_ptr<ClassLoader> udf_classloader;
    std::unique_ptr<ClassAnalyzer> analyzer;
    std::unique_ptr<UDFHelper> udf_helper;
    JVMClass udaf_class = nullptr;
    JVMClass udaf_state_class = nullptr;
    std::unique_ptr<JavaMethodDescriptor> create;
    std::unique_ptr<JavaMethodDescriptor> destory;
    std::unique_ptr<JavaMethodDescriptor> update;
    std::unique_ptr<JavaMethodDescriptor> merge;
    std::unique_ptr<JavaMethodDescriptor> finalize;
    std::unique_ptr<JavaMethodDescriptor> serialize;
    std::unique_ptr<JavaMethodDescriptor> serialize_size;

    std::unique_ptr<JavaMethodDescriptor> reset;
    std::unique_ptr<JavaMethodDescriptor> window_update;
    std::unique_ptr<JavaMethodDescriptor> get_values;

    std::unique_ptr<DirectByteBuffer> buffer;

    jobject handle;
    std::vector<uint8_t> buffer_data;

    std::unique_ptr<UDAFFunction> _func;
};

} // namespace starrocks::vectorized