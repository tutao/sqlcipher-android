/*
 * Copyright (C) 2006-2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
// modified from original source see README at the top level of this project

#undef LOG_TAG
#define LOG_TAG "CursorWindow"

#include "CursorWindow.h"
#include "ALog-priv.h"

#include <cassert>
#include <cstring>
#include <cstdlib>

namespace android {

    CursorWindow::CursorWindow(const char* name, void* data, size_t size, bool readOnly) :
            mData(data), mSize(size), mReadOnly(readOnly) {
        mName = strdup(name);
        mHeader = static_cast<Header*>(mData);
    }

    CursorWindow::~CursorWindow() {
        free(mName);
        free(mData);
    }

    status_t CursorWindow::create(const char* name, size_t size, CursorWindow** outWindow) {
        status_t result;
        size_t requestedSize = size;
        void* data = malloc(requestedSize);
        if (!data) {
            return NO_MEMORY;
        }
        auto* window = new CursorWindow(name, data, requestedSize, false);
        result = window->clear();
        if (!result) {
            ALOGD("Created new CursorWindow: freeOffset=%d, "
                   "numRows=%d, numColumns=%d, mSize=%zu, mData=%p",
                   window->mHeader->freeOffset,
                   window->mHeader->numRows,
                   window->mHeader->numColumns,
                   window->mSize, window->mData);
            *outWindow = window;
            return OK;
        }
        delete window;
        return result;
    }

    status_t CursorWindow::clear() {
        if (mReadOnly) {
            return INVALID_OPERATION;
        }
        mHeader->freeOffset = sizeof(Header) + sizeof(RowSlotChunk);
        mHeader->firstChunkOffset = sizeof(Header);
        mHeader->numRows = 0;
        mHeader->numColumns = 0;
        auto* firstChunk = static_cast<RowSlotChunk*>(offsetToPtr(mHeader->firstChunkOffset));
        firstChunk->nextChunkOffset = 0;
        return OK;
    }

    status_t CursorWindow::setNumColumns(uint32_t numColumns) {
        if (mReadOnly) {
            return INVALID_OPERATION;
        }
        uint32_t cur = mHeader->numColumns;
        if ((cur > 0 || mHeader->numRows > 0) && cur != numColumns) {
            ALOGE("Trying to go from %d columns to %d", cur, numColumns);
            return INVALID_OPERATION;
        }
        mHeader->numColumns = numColumns;
        return OK;
    }

    status_t CursorWindow::allocRow() {
        if (mReadOnly) {
            return INVALID_OPERATION;
        }
        // Fill in the row slot
        RowSlot* rowSlot = allocRowSlot();
        if (rowSlot == nullptr) {
            return NO_MEMORY;
        }
        // Allocate the slots for the field directory
        size_t fieldDirSize = mHeader->numColumns * sizeof(FieldSlot);
        uint32_t fieldDirOffset = alloc(fieldDirSize, true /*aligned*/);
        if (!fieldDirOffset) {
            mHeader->numRows--;
            ALOGD("The row failed, so back out the new row accounting "
                  "from allocRowSlot %d", mHeader->numRows);
            return NO_MEMORY;
        }

        auto* fieldDir = static_cast<FieldSlot*>(offsetToPtr(fieldDirOffset));
        memset(fieldDir, 0, fieldDirSize);
        rowSlot = getRowSlot(mHeader->numRows - 1);
        rowSlot->offset = fieldDirOffset;
        return OK;
    }

    status_t CursorWindow::freeLastRow() {
        if (mReadOnly) {
            return INVALID_OPERATION;
        }
        if (mHeader->numRows > 0) {
            mHeader->numRows--;
        }
        return OK;
    }

    status_t CursorWindow::maybeInflate() {
        size_t initialSize = 16 * 1024;
        size_t newSize = mSize <= initialSize
                         ? 2048 * 1024
                         : mSize * 2;
        uint32_t freeOffset = mHeader->freeOffset;
        ALOGI("Request to resize CursorWindow allocation: current window size %zu bytes, "
              "free space %zu bytes, new window size %zu bytes",
              mSize, freeSpace(), newSize);
        if((mData = realloc(mData, newSize))){
            mHeader = static_cast<Header*>(mData);
            mHeader->freeOffset = freeOffset;
            mSize = newSize;
            ALOGI("Resized CursorWindow allocation: current window size %zu bytes",
                  newSize);
            return OK;
        } else {
            ALOGE("Failed to resize CursorWindow allocation");
            return NO_MEMORY;
        }
    }

    uint32_t CursorWindow::alloc(size_t size, bool aligned) { // NOLINT(*-no-recursion)
        uint32_t padding;
        if (aligned) {
            // 4 byte alignment
            padding = (~mHeader->freeOffset + 1) & 3;
        } else {
            padding = 0;
        }
        uint32_t offset = mHeader->freeOffset + padding;
        uint32_t nextFreeOffset = offset + size;
        if (nextFreeOffset > mSize) {
            status_t result = maybeInflate();
            if(result == OK){
                return alloc(size, aligned);
            }
            ALOGI("Window is full: requested allocation %zu bytes, "
                  "free space %zu bytes, window size %zu bytes",
                  size, freeSpace(), mSize);
            return 0;
        }
        mHeader->freeOffset = nextFreeOffset;
        return offset;
    }

    CursorWindow::RowSlot* CursorWindow::getRowSlot(uint32_t row) {
        uint32_t chunkPos = row;
        auto* chunk = static_cast<RowSlotChunk*>(
                offsetToPtr(mHeader->firstChunkOffset));
        while (chunkPos >= ROW_SLOT_CHUNK_NUM_ROWS) {
            chunk = static_cast<RowSlotChunk*>(offsetToPtr(chunk->nextChunkOffset));
            chunkPos -= ROW_SLOT_CHUNK_NUM_ROWS;
        }
        return &chunk->slots[chunkPos];
    }

    CursorWindow::RowSlot* CursorWindow::allocRowSlot() {
        uint32_t chunkPos = mHeader->numRows;
        uint32_t chunkOffset = mHeader->firstChunkOffset;
        auto* chunk = static_cast<RowSlotChunk*>(
                offsetToPtr(mHeader->firstChunkOffset));
        while (chunkPos > ROW_SLOT_CHUNK_NUM_ROWS) {
            chunkOffset = chunk->nextChunkOffset;
            chunk = static_cast<RowSlotChunk*>(offsetToPtr(chunk->nextChunkOffset));
            chunkPos -= ROW_SLOT_CHUNK_NUM_ROWS;
        }
        if (chunkPos == ROW_SLOT_CHUNK_NUM_ROWS) {
            if (!chunk->nextChunkOffset) {
                uint32_t nextChunkOffset = alloc(sizeof(RowSlotChunk), true /*aligned*/);
                chunk = static_cast<RowSlotChunk*>(offsetToPtr(chunkOffset));
                chunk->nextChunkOffset = nextChunkOffset;
                if (!chunk->nextChunkOffset) {
                    return nullptr;
                }
            }
            chunk = static_cast<RowSlotChunk*>(offsetToPtr(chunk->nextChunkOffset));
            chunk->nextChunkOffset = 0;
            chunkPos = 0;
        }
        mHeader->numRows += 1;
        return &chunk->slots[chunkPos];
    }

    CursorWindow::FieldSlot* CursorWindow::getFieldSlot(uint32_t row, uint32_t column) {
        if (row >= mHeader->numRows || column >= mHeader->numColumns) {
            ALOGE("Failed to read row %d, column %d from a CursorWindow which "
                  "has %d rows, %d columns.",
                  row, column, mHeader->numRows, mHeader->numColumns);
            return nullptr;
        }
        RowSlot* rowSlot = getRowSlot(row);
        if (!rowSlot) {
            ALOGE("Failed to find rowSlot for row %d.", row);
            return nullptr;
        }
        auto* fieldDir = static_cast<FieldSlot*>(offsetToPtr(rowSlot->offset));
        return &fieldDir[column];
    }

    status_t CursorWindow::putBlob(uint32_t row, uint32_t column, const void* value, size_t size) {
        return putBlobOrString(row, column, value, size, FIELD_TYPE_BLOB);
    }

    status_t CursorWindow::putString(uint32_t row, uint32_t column, const char* value,
                                     size_t sizeIncludingNull) {
        return putBlobOrString(row, column, value, sizeIncludingNull, FIELD_TYPE_STRING);
    }

    status_t CursorWindow::putBlobOrString(uint32_t row, uint32_t column,
                                           const void* value, size_t size, int32_t type) {
        if (mReadOnly) {
            return INVALID_OPERATION;
        }
        uint32_t offset = alloc(size);
        if (!offset) {
            return NO_MEMORY;
        }
        memcpy(offsetToPtr(offset), value, size);
        FieldSlot* fieldSlot = getFieldSlot(row, column);
        if (!fieldSlot) {
            return BAD_VALUE;
        }
        fieldSlot->type = type;
        fieldSlot->data.buffer.offset = offset;
        fieldSlot->data.buffer.size = size;
        return OK;
    }

    status_t CursorWindow::putLong(uint32_t row, uint32_t column, int64_t value) {
        if (mReadOnly) {
            return INVALID_OPERATION;
        }
        FieldSlot* fieldSlot = getFieldSlot(row, column);
        if (!fieldSlot) {
            return BAD_VALUE;
        }
        fieldSlot->type = FIELD_TYPE_INTEGER;
        fieldSlot->data.l = value;
        return OK;
    }

    status_t CursorWindow::putDouble(uint32_t row, uint32_t column, double value) {
        if (mReadOnly) {
            return INVALID_OPERATION;
        }
        FieldSlot* fieldSlot = getFieldSlot(row, column);
        if (!fieldSlot) {
            return BAD_VALUE;
        }
        fieldSlot->type = FIELD_TYPE_FLOAT;
        fieldSlot->data.d = value;
        return OK;
    }

    status_t CursorWindow::putNull(uint32_t row, uint32_t column) {
        if (mReadOnly) {
            return INVALID_OPERATION;
        }
        FieldSlot* fieldSlot = getFieldSlot(row, column);
        if (!fieldSlot) {
            return BAD_VALUE;
        }
        fieldSlot->type = FIELD_TYPE_NULL;
        fieldSlot->data.buffer.offset = 0;
        fieldSlot->data.buffer.size = 0;
        return OK;
    }

} // namespace android
