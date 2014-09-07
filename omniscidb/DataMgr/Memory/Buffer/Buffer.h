/**
 * @file		Buffer.h
 * @author		Steven Stewart <steve@map-d.com>
 */
#ifndef DATAMGR_MEMORY_BUFFER_BUFFER_H
#define DATAMGR_MEMORY_BUFFER_BUFFER_H

#include <iostream>
#include "../AbstractDatum.h"

using namespace Memory_Namespace;

namespace Buffer_Namespace {
    
    /**
     * @struct  Page
     * @brief   A page holds a memory location and a "dirty" flag.
     */
    struct Page {
        mapd_addr_t addr = nullptr; /// memory address for beginning of page
        bool dirty = false;         /// indicates the page has been modified
        
        /// Constructor
        Page(const mapd_addr_t addrIn, const bool dirtyIn = false) : addr(addrIn), dirty(dirtyIn) {}
    };
    
    /**
     * @class   Buffer
     * @brief
     *
     * Note(s): Forbid Copying Idiom 4.1
     */
    class Buffer : public AbstractDatum {
        friend class BufferMgr;
        
    public:
        
        /**
         * @brief Constructs a Buffer object.
         * The constructor requires a memory address (provided by BufferMgr), number of pages, and
         * the size in bytes of each page. Additionally, the Buffer can be initialized with an epoch.
         *
         * @param mem       The beginning memory address of the buffer.
         * @param numPages  The number of pages into which the buffer's memory space is divided.
         * @param pageSize  The size in bytes of each page that composes the buffer.
         * @param epoch     A temporal reference implying the buffer is up-to-date up to the epoch.
         */
        Buffer(const mapd_addr_t mem, const mapd_size_t numPages, const mapd_size_t pageSize, const int epoch);
        
        /// Destructor
        virtual ~Buffer();
        
        /**
         * @brief Reads (copies) data from the buffer to the destination (dst) memory location.
         * Reads (copies) nbytes of data from the buffer, beginning at the specified byte offset,
         * into the destination (dst) memory location.
         *
         * @param dst       The destination address to where the buffer's data is being copied.
         * @param offset    The byte offset into the buffer from where reading (copying) begins.
         * @param nbytes    The number of bytes being read (copied) into the destination (dst).
         */
        virtual void read(mapd_addr_t const dst, const mapd_size_t offset, const mapd_size_t nbytes = 0);
        
        /**
         * @brief Writes (copies) data from src into the buffer.
         * Writes (copies) nbytes of data into the buffer at the specified byte offset, from
         * the source (src) memory location.
         *
         * @param src       The source address from where data is being copied to the buffer.
         * @param offset    The byte offset into the buffer to where writing begins.
         * @param nbytes    The number of bytes being written (copied) into the buffer.
         */
        virtual void write(mapd_addr_t src, const mapd_size_t offset, const mapd_size_t nbytes);
        
        /**
         * @brief Appends nbytes from src to the end of the buffer
         * Appends nbytes from src to the end of the buffer.
         *
         * @param src       The source address from where data is being appended to the buffer.
         * @param nbytes    The number of bytes being written (appended) to the buffer.
         */
        virtual void append(mapd_addr_t src, const mapd_size_t nbytes);
        
        /**
         * @brief Returns a raw, constant (read-only) pointer to the underlying buffer.
         * @return A constant memory pointer for read-only access.
         */
        virtual const mapd_byte_t* getMemoryPtr() const;
        
        /// Returns the number of pages in the buffer.
        virtual mapd_size_t pageCount() const;
        
        /// Returns the size in bytes of each page in the buffer.
        virtual mapd_size_t pageSize() const;
        
        /// Returns the total number of bytes allocated for the buffer.
        virtual mapd_size_t size() const;
        
        /// Returns the total number of used bytes in the buffer.
        virtual mapd_size_t used() const;
        
        /// Returns whether or not the buffer has been modified since the last flush/checkpoint.
        virtual bool isDirty() const;
        
    private:
        Buffer(const Buffer&);      // private copy constructor
        Buffer& operator=(const Buffer&); // private overloaded assignment operator
        mapd_addr_t mem_;           /// pointer to beginning of datum's memory
        mapd_size_t nbytes_;        /// total number of bytes allocated for head pointer
        mapd_size_t used_;          /// total number of used bytes in the datum
        mapd_size_t pageSize_;      /// the size of each page in the datum buffer
        int epoch_;                 /// indicates when the datum was last flushed
        bool dirty_;                /// true if buffer has been modified
        std::vector<Page> pages_;   /// a vector of pages (page metadata) that compose the buffer
    };
    
} // Buffer_Namespace

#endif // DATAMGR_MEMORY_BUFFER_BUFFER_H