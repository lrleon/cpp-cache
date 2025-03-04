
// #include <lz4.h>
// #include <cereal/archives/binary.hpp>
// #include <cereal/types/vector.hpp>
// #include <cereal/types/memory.hpp>
// #include <sstream>

// using namespace std;

// // ==================== Serialization ====================

// template<typename T>
// vector<char> serializeWithCereal(const T& obj) {
//     ostringstream oss;
//     cereal::BinaryOutputArchive archive(oss);
//     archive(obj);
//     string str = oss.str();
//     return vector<char>(str.begin(), str.end());
// }

// template<typename T>
// T deserializeWithCereal(const vector<char>& byteArray) {
//     istringstream iss(string(byteArray.begin(), byteArray.end()));
//     cereal::BinaryInputArchive archive(iss);
//     T obj;
//     archive(obj);
//     return obj;
// }

// class Serializable {
//   public:
//     virtual vector<char> serialize() const = 0;
//     virtual void deserialize(const vector<char>& data) = 0;
//     virtual ~Serializable() = default;
// };

// // implement serializable interface for native types (int, float, double, etc.)
// template <class T>
// class TypeWrapper : Serializable {
//   T value;
//   public:
//     TypeWrapper(const T& value) : value(value) {}
//     TypeWrapper() {}
//     operator T() const { return value; }
//     TypeWrapper& operator=(const T& value) { this->value = value; return *this; }
    
//     template<class Archive>
//     void serialize(Archive& archive) {
//       archive(value);
//     }

//     vector<char> serialize() const override {
//       return serializeWithCereal(*this);
//     }

//     void deserialize(const vector<char>& data) override {
//       *this = deserializeWithCereal<TypeWrapper<T>>(data);
//     }
  
// };


// // ==================== Compression ====================

 
// void lz4_compress(const vector<char>& in, vector<char>& out) {
//     if (out.size() < LZ4_compressBound(in.size())) {
//         out.resize(LZ4_compressBound(in.size())); // Ensure the output vector<char> is large enough
//     }

//     int compressedSize = LZ4_compress_default(in.data(), out.data(), in.size(), out.size());
//     if (compressedSize < 1) {
//         cerr << "Compression failed!" << endl;
//     } else {
//         out.resize(compressedSize); // Resize the output vector<char> to the actual compressed size
//     }
// }
 
// void lz4_decompress(const vector<char>& in, vector<char>& out) {
//     // The output vector<char> must be pre-sized to the expected decompressed size
//     int decompressedSize = LZ4_decompress_safe(in.data(), out.data(), in.size(), out.size());
//     if (decompressedSize < 0) {
//         cerr << "Decompression failed!" << endl;
//     } else {
//         out.resize(decompressedSize); // Resize the output vector<char> to the actual decompressed size
//     }
// }


