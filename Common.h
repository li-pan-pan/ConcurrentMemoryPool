﻿#pragma once

#include<iostream>
#include<algorithm>
#include<vector>
#include<atomic>
#include<unordered_map>
#include<map>
#include<thread>
#include<mutex>
#include<time.h>
#include<assert.h>
using std::cout;
using std::endl;

#ifdef _WIN32
#include<windows.h>
#else
//...
#endif


static const size_t MAX_BYTES = 256 * 1024;
static const size_t NFREELIST = 208;
static const size_t NPAGES=129;
static const size_t PAGE_SHIFT = 13;

#ifdef _WIN64
    typedef unsigned long long PAGE_ID;
#elif _WIN32
	typedef size_t PAGE_ID;
#elif
    //linux
#endif //_WIN32

// 直接去堆上按页申请空间
inline static void* SystemAlloc(size_t kpage)
{
#ifdef _WIN32 
	void* ptr = VirtualAlloc(0, kpage*8*1024,MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else 
	// linux下brk mmap等
#endif 
	if (ptr == nullptr) 
		throw std::bad_alloc(); 
	return ptr;
}

inline static void SystemFree(void* ptr)
{
#ifdef _WIN32
	VirtualFree(ptr, 0, MEM_RELEASE);
#else
	// sbrk unmmap等
#endif
}

 static void*& NextObj(void* obj)
{
	return *(void**)obj;
}

//管理切分好的小对象的自由链表
class FreeList
{
public:
	void Push(void* obj)
	{
		assert(obj);
		//头插
		//*(void**)obj = _freeList;
		NextObj(obj) = _freeList;
		_freeList = obj;

		++_size;
	}

	void PushRange(void* start, void* end,size_t n)
	{
		NextObj(end) = _freeList;
		_freeList = start;

		_size += n;
	}

	void PopRange(void*& start, void*& end, size_t n)
	{
		assert(n >= _size);
		start = _freeList;
		end = start;
		for (size_t i = 0; i < n - 1; ++i)
		{
			end = NextObj(end);
		}
		_freeList = NextObj(end);
		NextObj(end) = nullptr;

		_size -= n;
	}

	void* Pop()
	{
		assert(_freeList);
		//头删
		void* obj = _freeList;
		_freeList = NextObj(obj);

		--_size;

		return obj;
	}

	bool Empty()
	{
		return _freeList == nullptr;
	}

	size_t& MaxSize()
	{
		return _maxSize;
	}

	size_t Size()
	{
		return _size;
	}
private:
	void* _freeList = nullptr;
	size_t _maxSize = 1;
	size_t _size;

};

//计算对象大小的对其映射规则
// 整体控制在最多10%左右的内碎片浪费 
// [1,128]                      8byte对齐        freelist[0,16) 
// [128+1,1024]                 16byte对齐       freelist[16,72) 
// [1024+1,8*1024]              128byte对齐      freelist[72,128) 
// [8*1024+1,64*1024]           1024byte对齐     freelist[128,184) 
// [64*1024+1,256*1024]         8*1024byte对齐   freelist[184,208)
class SizeClass
{
public:

	/*size_t _RoundUp(size_t size,size_t AlignNum)
	{
		size_t alignSize;
		if (size % AlignNum != 0)
		{
			alignSize = (size / AlignNum + 1)*AlignNum;
		}
		else
		{
			alignSize = size;
		}
		return alignSize;
	}*/
	static inline size_t _RoundUp(size_t bytes, size_t alignNum)
	{
		return ((bytes + alignNum - 1)&~(alignNum - 1));
	}
	static inline size_t RoundUp(size_t size)
	{
		if (size <= 128)
		{
			return _RoundUp(size, 8);
		}
		else if (size <= 1024)
		{
			return _RoundUp(size, 16);
		}
		else if (size <= 1024 * 8)
		{
			return _RoundUp(size, 128);
		}
		else if (size <= 1024 * 64)
		{
			return _RoundUp(size, 1024);
		}
		else if (size <= 1024 * 256)
		{
			return _RoundUp(size, 8*1024);
		}
		else
		{
			return _RoundUp(size, 1 << PAGE_SHIFT);
		}
	}
	/*size_t _Index(size_t bytes, size_t alignNum)
	{
		if (bytes % alignNum == 0)
		{
			return bytes / alignNum - 1;
		}
		else
		{
			return bytes / alignNum;
		}
	}*/
	static inline size_t _Index(size_t bytes, size_t align_shift) 
	{ 
		return ((bytes + (1 << align_shift) - 1) >> align_shift) - 1; 
	}
	// 计算映射的哪一个自由链表桶 
	static inline size_t Index(size_t bytes) 
	{
		assert(bytes <= MAX_BYTES); // 每个区间有多少个链 
		static int group_array[4] = { 16, 56, 56, 56 }; 
		if (bytes <= 128)
		{
			return _Index(bytes, 3);
		}
		else if (bytes <= 1024)
		{ 
			return _Index(bytes - 128, 4) + group_array[0]; 
		}
		else if (bytes <= 8 * 1024)
		{ 
			return _Index(bytes - 1024, 7) + group_array[1] + group_array[0]; 
		}
		else if (bytes <= 64 * 1024)
		{ 
			return _Index(bytes - 8 * 1024, 10) + group_array[2] + group_array[1] + group_array[0]; 
		}
		else if (bytes <= 256 * 1024)
		{ 
			return _Index(bytes - 64 * 1024, 13) + group_array[3] + group_array[2] + group_array[1] + group_array[0]; 
		}
		else
		{ 
			assert(false); 
		} 
		return -1;
	}

	// 一次从中心缓存获取多少个 
	static size_t NumMoveSize(size_t size) 
	{ 
		assert(size > 0);
		// [2, 512]，一次批量移动多少个对象的(慢启动)上限值 
		// 小对象一次批量上限高 
		// 小对象一次批量上限低 
		int num = MAX_BYTES / size; 

		if (num < 2) 
			num = 2; 

		if (num > 512) 
			num = 512; 

		return num; 
	}
	// 计算一次向系统获取几个页
	// 单个对象 8byte 
	// ... 
	// 单个对象 256KB 
	static size_t NumMovePage(size_t size)
	{
		size_t num = NumMoveSize(size);
		size_t npage = num*size; 

		npage >>= PAGE_SHIFT; 
		if (npage == 0) 
			npage = 1; 
		return npage;
	}
private:
};

//管理连续页大块内存跨度
struct Span
{
	PAGE_ID _pageId;//大块内存起始页的页号
	size_t _n;      //页的数量

	Span* _next = nullptr;    //双向链表的结构
	Span* _prev = nullptr;

	size_t _objSize = 0;   //切好的小对象的大小
	size_t _useCount = 0 ;//切好小块内存，被分配给thread cache的计数
	void* _freeList = nullptr; //切好小块内存的自由链表

	bool _isUse=false;    //是否被使用
};

//带头双向循环链表
class SpanList
{
public:
	SpanList()
	{
		_head = new Span;
		_head->_next = _head;
		_head->_prev = _head;
	}

	Span* Begin()
	{
		return _head->_next;
	}

	Span* End()
	{
		return _head;
	}

	bool Empty()
	{
		return _head->_next == _head;
	}


	void PushFront(Span* span)
	{
		Insert(Begin(), span);
	}

	Span* PopFront()
	{
		Span* front = _head->_next;
		Erase(front);
		return front;
	}

	void Insert(Span* pos, Span* newSpan)
	{
		assert(pos);
		assert(newSpan);

		Span* prev = pos->_prev;
		//prev newspan pos
		prev->_next = newSpan;
		newSpan->_next = pos;
		newSpan->_prev = prev;
		pos->_prev = newSpan;
	}

	void Erase(Span* pos)
	{
		assert(pos);
		assert(pos != _head);
		//条件断点
		/*if (pos == _head)
		{

		}
*/
		Span* prev = pos->_prev;
		Span* next = pos->_next;

		prev->_freeList = next;
		next->_prev = prev;
	}
private:
	Span* _head;
public:
	std::mutex _mtx;
};