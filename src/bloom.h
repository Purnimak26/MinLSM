#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace lsm {

class Bloom {
public:
    Bloom() = default;

    static Bloom optimal(std::size_t capacity, double fpr) {
        if (capacity == 0) throw std::invalid_argument("Capacity must be greater than 0");
        if (fpr <= 0.0 || fpr >= 1.0) throw std::invalid_argument("False Positive Rate must be in (0,1)");

        const double ln2 = std::log(2.0);
        const double m = std::ceil(-static_cast<double>(capacity) * std::log(fpr) / (ln2 * ln2));
        const std::size_t mb = static_cast<std::size_t>(m);
        const std::size_t kb = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::round(m / static_cast<double>(capacity) * ln2)));

        Bloom b;
        b.msize = mb;
        b.ksize = kb;
        b.bitsize.assign((mb + 7) / 8, 0);
        return b;
    }

    void add(std::string_view key) {
        if (msize == 0) return;
        std::uint64_t h1, h2;
        hash128(key, h1, h2);
        for (std::size_t i = 0; i < ksize; i++) {
            const std::size_t pos = (h1 + i * h2) % msize;
            bitsize[pos >> 3] |= static_cast<std::uint8_t>(1u << (pos & 7));
        }
    }

    bool maybe_present(std::string_view key) const {
        if (msize == 0) return true;  
        std::uint64_t h1, h2;
        hash128(key, h1, h2);
        for (std::size_t i = 0; i < ksize; i++) {
            const std::size_t pos = (h1 + i * h2) % msize;
            if (!(bitsize[pos >> 3] & (1u << (pos & 7)))) return false;
        }
        return true;
    }

    std::vector<std::uint8_t> to_bytes() const {
        std::vector<std::uint8_t> out;
        out.reserve(16 + bitsize.size());
        put_u64(out, msize);
        put_u64(out, ksize);
        out.insert(out.end(), bitsize.begin(), bitsize.end());
        return out;
    }

    static Bloom from_bytes(const std::uint8_t* data, std::size_t n) {
        if (n < 16) throw std::runtime_error("Bloom: data too short");
        Bloom b;
        b.msize = get_u64(data);
        b.ksize = get_u64(data + 8);
        const std::size_t nbytes = (b.msize + 7) / 8;
        if (n != 16 + nbytes) throw std::runtime_error("Bloom: size mismatch");
        b.bitsize.assign(data + 16, data + 16 + nbytes);
        return b;
    }

    std::size_t size_bytes() const { return bitsize.size(); }

private:
    
    static void hash128(std::string_view key, std::uint64_t& h1, std::uint64_t& h2) {
        std::uint64_t h = 1469598103934665603ull;
        for (unsigned char c : key) {
            h ^= c;
            h *= 1099511628211ull;
        }
        h1 = h;
        h2 = (h * 0x9E3779B97F4A7C15ull) | 1;  
    }

    static void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
    static std::uint64_t get_u64(const std::uint8_t* p) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
        return v;
    }

    std::size_t msize = 0;                
    std::size_t ksize = 0;                 
    std::vector<std::uint8_t> bitsize;     
};

}  