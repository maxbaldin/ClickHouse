#pragma once

#include <DB/IO/CompressedReadBufferBase.h>
#include "ReadBufferFromFile.h"


namespace DB
{

/// В отличие от CompressedReadBuffer, умеет делать seek.
class CompressedReadBufferFromFile : public CompressedReadBufferBase, public BufferWithOwnMemory<ReadBuffer>
{
private:
	/** В любой момент выполняется одно из двух:
	  * a) size_compressed = 0
	  * b)
	  *  - working_buffer содержит целиком один блок.
	  *  - file_in смотрит в конец этого блока.
	  *  - size_compressed содержит сжатый размер этого блока.
	  */
	ReadBufferFromFile file_in;
	size_t size_compressed;

	bool nextImpl()
	{
		size_t size_decompressed;
		size_compressed = readCompressedData(size_decompressed);
		if (!size_compressed)
			return false;

		memory.resize(size_decompressed);
		working_buffer = Buffer(&memory[0], &memory[size_decompressed]);

		decompress(working_buffer.begin(), size_decompressed);

		return true;
	}

public:
	CompressedReadBufferFromFile(const std::string & path, size_t buf_size = DBMS_DEFAULT_BUFFER_SIZE)
		: BufferWithOwnMemory<ReadBuffer>(0), file_in(path, buf_size)
	{
		compressed_in = &file_in;
	}

	void seek(size_t offset_in_compressed_file, size_t offset_in_decompressed_block)
	{
		if (size_compressed &&
			offset_in_compressed_file == file_in.getPositionInFile() - size_compressed &&
			offset_in_decompressed_block <= working_buffer.size())
		{
			bytes += offset();
			pos = working_buffer.begin() + offset_in_decompressed_block;
			/// bytes может переполниться и получиться отрицательным, но в count() все переполнится обратно и получится правильно.
			bytes -= offset();
		}
		else
		{
			file_in.seek(offset_in_compressed_file);

			bytes += offset();
			nextImpl();

			if (offset_in_decompressed_block > working_buffer.size())
				throw Exception("Seek position is beyond the decompressed block", ErrorCodes::ARGUMENT_OUT_OF_BOUND);

			pos = working_buffer.begin() + offset_in_decompressed_block;
			bytes -= offset();
		}
	}

	size_t readBig(char * to, size_t n)
	{
		size_t bytes_read = 0;

		/// Если в буфере есть непрочитанные байты, то скопируем сколько надо в to.
		if (pos < working_buffer.end())
			bytes_read += read(to, std::min(static_cast<size_t>(working_buffer.end() - pos), n));

		/// Если надо ещё прочитать - будем, по возможности, разжимать сразу в to.
		while (bytes_read < n)
		{
			size_t size_decompressed = 0;

			size_t new_size_compressed = readCompressedData(size_decompressed);
			size_compressed = 0; /// file_in больше не указывает на конец блока в working_buffer.
			if (!new_size_compressed)
				return bytes_read;

			/// Если разжатый блок помещается целиком туда, куда его надо скопировать.
			if (size_decompressed <= n - bytes_read)
			{
				decompress(to + bytes_read, size_decompressed);
				bytes_read += size_decompressed;
				bytes += size_decompressed;
			}
			else
			{
				size_compressed = new_size_compressed;
				bytes += offset();
				memory.resize(size_decompressed);
				working_buffer = Buffer(&memory[0], &memory[size_decompressed]);
				pos = working_buffer.begin();

				decompress(working_buffer.begin(), size_decompressed);

				bytes_read += read(to + bytes_read, n - bytes_read);
				break;
			}
		}

		return bytes_read;
	}
};

}