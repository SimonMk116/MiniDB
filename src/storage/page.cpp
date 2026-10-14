#include "minidb/storage/page.hpp"

namespace minidb
{

    Page::Page()
    {
        reset();
    }

    std::byte *Page::data()
    {
        return data_.data();
    }

    const std::byte *Page::data() const
    {
        return data_.data();
    }

    void Page::reset()
    {
        data_.fill(std::byte{0});
    }

} // namespace minidb