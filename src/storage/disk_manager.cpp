#include "minidb/storage/disk_manager.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace minidb
{
    DiskManager::DiskManager(const std::string &filename, std::unique_ptr<FreeList> free_list)
        : free_list_(std::move(free_list))
    {
        if (!free_list_)
        {
            throw std::invalid_argument("DiskManager requires a free-list strategy");
        }

        file_.open(filename, std::ios::in | std::ios::out | std::ios::binary);
        // create file if it does not exist yet
        if (!file_)
        {
            file_.clear();
            file_.open(filename, std::ios::out | std::ios::binary);
            file_.close();
            file_.open(filename, std::ios::in | std::ios::out | std::ios::binary);
        }
        if (!file_)
        {
            throw std::runtime_error("Could not open database file");
        }

        file_.seekg(0, std::ios::end);
        const auto size = file_.tellg();
        if (size < 0)
        {
            throw std::runtime_error("Could not determine database file size");
        }

        if (static_cast<std::size_t>(size) % PAGE_SIZE != 0)
        {
            throw std::runtime_error("Database file size is not a multiple of PAGE_SIZE");
        }

        const auto page_count = static_cast<std::uintmax_t>(size) / PAGE_SIZE;
        if (page_count > std::numeric_limits<page_id_t>::max())
        {
            throw std::runtime_error("Database contains more pages than page_id_t can address");
        }
        next_page_id_ = static_cast<page_id_t>(page_count);
    }

    DiskManager::~DiskManager()
    {
        if (file_.is_open())
        {
            file_.close();
        }
    }

    void DiskManager::readPage(page_id_t page_id, Page &page)
    {
        if (page_id >= next_page_id_)
        {
            throw std::out_of_range("page_id is out of range for this database file");
        }

        const auto offset = static_cast<std::streamoff>(page_id) * PAGE_SIZE;
        file_.seekg(offset);
        if (!file_)
        {
            throw std::runtime_error("Failed to seek in database file while reading page");
        }

        file_.read(reinterpret_cast<char *>(page.data()), static_cast<std::streamsize>(PAGE_SIZE));
        if (!file_ && file_.gcount() != static_cast<std::streamsize>(PAGE_SIZE))
        {
            throw std::runtime_error("Failed to read full page from disk");
        }
    }

    void DiskManager::writePage(page_id_t page_id, const Page &page)
    {
        if (page_id >= next_page_id_)
        {
            throw std::out_of_range("page_id is out of range for this database file");
        }

        const auto offset = static_cast<std::streamoff>(page_id) * PAGE_SIZE;
        file_.seekp(offset);
        if (!file_)
        {
            throw std::runtime_error("Failed to seek in database file while writing page");
        }

        file_.write(reinterpret_cast<const char *>(page.data()), static_cast<std::streamsize>(PAGE_SIZE));
        if (!file_)
        {
            throw std::runtime_error("Failed to write full page to disk");
        }

        file_.flush();
        if (!file_)
        {
            throw std::runtime_error("Failed to flush page to disk");
        }
    }

    page_id_t DiskManager::allocatePage()
    {
        if (const auto free_page_id = free_list_->acquirePage())
        {
            Page empty_page;
            try
            {
                writePage(*free_page_id, empty_page);
            }
            catch (...)
            {
                free_list_->releasePage(*free_page_id);
                throw;
            }
            return *free_page_id;
        }

        if (next_page_id_ == std::numeric_limits<page_id_t>::max())
        {
            throw std::overflow_error("Database has reached the maximum addressable page count");
        }

        const auto page_id = next_page_id_;

        file_.seekp(static_cast<std::streamoff>(page_id) * PAGE_SIZE);
        if (!file_)
        {
            throw std::runtime_error("Failed to seek to end of database file while allocating page");
        }

        static const std::array<std::byte, PAGE_SIZE> zero_page{};
        file_.write(reinterpret_cast<const char *>(zero_page.data()), static_cast<std::streamsize>(PAGE_SIZE));
        if (!file_)
        {
            throw std::runtime_error("Failed to extend database file while allocating page");
        }

        file_.flush();
        if (!file_)
        {
            throw std::runtime_error("Failed to flush database file while allocating page");
        }

        next_page_id_ = page_id + 1;
        return page_id;
    }

    void DiskManager::freePage(page_id_t page_id)
    {
        if (page_id >= next_page_id_)
        {
            throw std::out_of_range("page_id is out of range for this database file");
        }

        free_list_->releasePage(page_id);
    }

}
