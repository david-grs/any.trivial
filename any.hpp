#pragma once

#include <array>
#include <memory>
#include <cstring>
#include <type_traits>
#include <typeinfo>
#include <typeindex>
#include <cassert>
#include <sstream>
#include <string>

namespace detail { namespace static_any {

// Pointer to administrative function, function that will by type-specific, and will be able to perform all the required operations
enum class operation_t { query_type, query_size, query_noexcept, copy, move, destroy };

using function_ptr_t = void(*)(operation_t operation, void* this_ptr, void* other_ptr);

template<typename _T>
static void operation(operation_t operation, void* ptr1, void* ptr2)
{
	_T* this_ptr = reinterpret_cast<_T*>(ptr1);

	switch(operation)
	{
		case operation_t::query_type:
		{
			*reinterpret_cast<const std::type_info**>(ptr1) = &typeid(_T);
			break;
		}
		case operation_t::query_size:
		{
			*reinterpret_cast<std::size_t*>(ptr1) = sizeof(_T);
			break;
		}
		case operation_t::query_noexcept:
		{
			*reinterpret_cast<bool*>(ptr1) = std::is_nothrow_copy_constructible<_T>::value || std::is_nothrow_move_constructible<_T>::value;
			break;
		}
		case operation_t::copy:
		{
			_T* other_ptr = reinterpret_cast<_T*>(ptr2);
			assert(this_ptr);
			assert(other_ptr);
			new(this_ptr)_T(*other_ptr);
			break;
		}
		case operation_t::move:
		{
			_T* other_ptr = reinterpret_cast<_T*>(ptr2);
			assert(this_ptr);
			assert(other_ptr);
			new(this_ptr)_T(std::move(*other_ptr));
			break;
		}
		case operation_t::destroy:
		{
			assert(this_ptr);
			this_ptr->~_T();
			break;
		}
	}
}

template<typename _T>
static function_ptr_t get_function_for_type()
{
	return &static_any::operation<std::remove_cv_t<std::remove_reference_t<_T>>>;
}

}}

template <std::size_t _N>
struct static_any
{
	typedef std::size_t size_type;
	typedef std::aligned_storage_t<_N> storage_t;

	static_any() = default;

	~static_any()
	{
		destroy();
	}

	template<typename _T>
	static_any(_T&& v)
	{
		copy_or_move(std::forward<_T>(v));
	}

	template<std::size_t _M>
	static_any(const static_any<_M>& another)
	{
		copy_or_move_from_another(another);
	}

	template<std::size_t _M>
	static_any(static_any<_M>& another)
	{
		copy_or_move_from_another(std::forward<static_any<_M>>(another));
	}

	template<std::size_t _M>
	static_any(static_any<_M>&& another)
	{
		copy_or_move_from_another(std::forward<static_any<_M>>(another));
	}

	template<std::size_t _M>
	static_any& operator=(const static_any<_M>& another)
	{
		assign_from_another(another);
		return *this;
	}

	template<std::size_t _M>
	static_any& operator=(static_any<_M>& another)
	{
		assign_from_another(another);
		return *this;
	}

	template<std::size_t _M>
	static_any& operator=(static_any<_M>&& another)
	{
		assign_from_another(std::forward<static_any<_M>>(another));
		return *this;
	}

	template <typename _T>
	static_any& operator=(const _T& t)
	{
		assign(std::forward<_T>(t));
		return *this;
	}

	template <typename _T>
	static_any& operator=(_T&& t)
	{
		assign(std::forward<_T>(t));
		return *this;
	}

	inline void reset() { destroy(); }

	template<typename _T>
	inline const _T& get() const;

	template<typename _T>
	inline _T& get();

	template <typename _T>
	bool has() const
	{
		if (function_ == detail::static_any::get_function_for_type<_T>())
		{
			return true;
		}
		else if (function_)
		{
			// need to try another, possibly more costly way, as we may compare types across DLL boundaries
			return std::type_index(typeid(_T)) == std::type_index(query_type());
		}
		return false;
	}

	const std::type_info& type() const
	{
		if (empty())
			return typeid(void);
		else
			return query_type();
	}

	bool empty() const { return function_ == nullptr; }

	size_type size() const
	{
		if (empty())
			return 0;
		else
			return query_size();
	}

	static constexpr size_type capacity() { return _N; }

	// Initializes with object of type T, created in-place with specified constructor params
	template<typename _T, typename... Args>
	void emplace(Args&&... args)
	{
		static_any<_N> old;
		// If we contain a type, keep in case _T construction fails
		if(!empty() && query_noexcept())
		{
			old = std::move(*this);
		}
		destroy();
		try {
			new(&buff_) _T(std::forward<Args>(args)...);
		}
		catch(...) {
			if(!old.empty()) {
				*this = std::move(old);
			}
			throw;
		}
		function_ = detail::static_any::get_function_for_type<_T>();
	}

private:
	using operation_t = detail::static_any::operation_t;
	using function_ptr_t = detail::static_any::function_ptr_t;

	template <typename _T>
	void copy_or_move(_T&& t)
	{
		static_assert(capacity() >= sizeof(_T), "_T is too big to be copied to static_any");
		assert(function_ == nullptr);

		using NonConstT = std::remove_cv_t<std::remove_reference_t<_T>>;
		NonConstT* non_const_t = const_cast<NonConstT*>(&t);

		try {
			call_copy_or_move<_T&&>(&buff_, non_const_t);
		}
		catch(...) {
			throw;
		}

		function_ = detail::static_any::get_function_for_type<_T>();
	}

	template <typename _T>
	void assign(_T&& t)
	{
		static_assert(capacity() >= sizeof(_T), "_T is too big to be copied to static_any");

		using NonConstT = std::remove_cv_t<std::remove_reference_t<_T>>;
		NonConstT* non_const_t = const_cast<NonConstT*>(&t);

		storage_t old_buff;
		function_ptr_t old_function;
		constexpr bool cannot_throw =
			std::conditional<std::is_rvalue_reference<_T>::value,
				typename std::is_nothrow_move_constructible<_T>,
				typename std::is_nothrow_copy_constructible<_T>>::type::value;

		if(!cannot_throw) {
			old_buff = buff_;
			old_function = function_;
		}

		destroy();

		try {
			call_copy_or_move<_T&&>(&buff_, non_const_t);
		}
		catch(...) {
			if(!cannot_throw) {
				buff_ = old_buff;
				function_ = old_function;
			}
			throw;
		}

		function_ = detail::static_any::get_function_for_type<_T>();
	}

	template <typename _RefT>
	std::enable_if_t<std::is_rvalue_reference<_RefT>::value>
	call_copy_or_move(void* this_void_ptr, void* other_void_ptr)
	{
		detail::static_any::get_function_for_type<_RefT>()(operation_t::move, this_void_ptr, other_void_ptr);
	}

	template <typename _RefT>
	std::enable_if_t<!std::is_rvalue_reference<_RefT>::value>
	call_copy_or_move(void* this_void_ptr, void* other_void_ptr)
	{
		detail::static_any::get_function_for_type<_RefT>()(operation_t::copy, this_void_ptr, other_void_ptr);
	}

	const std::type_info& query_type() const
	{
		assert(function_ != nullptr);
		const std::type_info* ti ;
		function_(operation_t::query_type, &ti, nullptr);
		return *ti;
	}

	size_type query_size() const
	{
		assert(function_ != nullptr);
		std::size_t size;
		function_(operation_t::query_size, &size, nullptr);
		return size;
	}

	bool query_noexcept() const
	{
		bool can_noexcept;
		function_(operation_t::query_noexcept, &can_noexcept, nullptr);
		return can_noexcept;
	}

	void destroy()
	{
		if (function_)
		{
			void* not_used = nullptr;
			function_(operation_t::destroy, &buff_, not_used);
			function_ = nullptr;
		}
	}

	template<typename _T>
	const _T* as() const
	{
		return reinterpret_cast<const _T*>(&buff_);
	}

	template<typename _T>
	_T* as()
	{
		return reinterpret_cast<_T*>(&buff_);
	}

	template <typename _RefT>
	std::enable_if_t<std::is_rvalue_reference<_RefT>::value>
	call_copy_or_move(const function_ptr_t& function, void* this_void_ptr, void* other_void_ptr)
	{
		function(operation_t::move, this_void_ptr, other_void_ptr);
	}

	template <typename _RefT>
	std::enable_if_t<!std::is_rvalue_reference<_RefT>::value>
	call_copy_or_move(const function_ptr_t& function, void* this_void_ptr, void* other_void_ptr)
	{
		function(operation_t::copy, this_void_ptr, other_void_ptr);
	}

	template<std::size_t _M,
			 typename _X=std::enable_if_t<_M <= _N>>
	void copy_or_move_from_another(static_any<_M>&& another)
	{
		assert(function_ == nullptr);

		if (another.function_ == nullptr)
			return;

		void* other_data = reinterpret_cast<void*>(const_cast<decltype(another.buff_)*>(&another.buff_));

		try {
			call_copy_or_move<static_any<_M>&&>(another.function_, &buff_, other_data);
		}
		catch(...) {
			throw;
		}

		function_= another.function_;
	}

	template<typename _T>
	void assign_from_another(_T&& another)
	{
		// This function is only called from functions taking static_any<_M>
		constexpr auto _M = std::decay<_T>::type::capacity();
		static_assert(_M <= _N, "cannot assign from larger static_any");

		if (another.function_ == nullptr) {
			destroy();
			return;
		}

		bool nothrow_copy, nothrow_move;
		storage_t old_buff;
		function_ptr_t old_function = nullptr;
		another.function_(operation_t::query_noexcept, &nothrow_copy, &nothrow_move);

		if(!nothrow_copy) {
			 old_buff = buff_;
			 old_function = function_;
		}

		void* other_data = reinterpret_cast<void*>(const_cast<decltype(another.buff_)*>(&another.buff_));

		destroy();

		try {
			if(std::is_rvalue_reference<decltype(another)>::value) {
				another.function_(operation_t::move, &buff_, other_data);
			}
			else {
				another.function_(operation_t::copy, &buff_, other_data);
			}
		}
		catch(...) {
			if(!nothrow_copy) {
				buff_ = old_buff;
				function_ = old_function;
			}
			throw;
		}

		function_= another.function_;
	}

	storage_t buff_;
	function_ptr_t function_ = nullptr;

	template<std::size_t _S>
	friend struct static_any;

	template<typename _ValueT, std::size_t _S>
	friend _ValueT* any_cast(static_any<_S>*);

	template<typename _ValueT, std::size_t _S>
	friend _ValueT& any_cast(static_any<_S>&);
};

struct bad_any_cast : public std::bad_cast
{
	bad_any_cast(const std::type_info& from,
				 const std::type_info& to);

	virtual ~bad_any_cast();

	bad_any_cast(const bad_any_cast&) =default;

	const std::type_info& stored_type() const { return from_; }
	const std::type_info& target_type() const { return to_; }

	virtual const char* what() const noexcept override
	{
		return reason_.c_str();
	}

private:
	const std::type_info& from_;
	const std::type_info& to_;
	std::string reason_;
};

inline bad_any_cast::bad_any_cast(const std::type_info& from,
			 const std::type_info& to)
: from_(from),
  to_(to)
{
	std::ostringstream oss;
	oss << "failed conversion using any_cast: stored type "
		<< from.name()
		<< ", trying to cast to "
		<< to.name();
	reason_ = oss.str();
}

inline bad_any_cast::~bad_any_cast() {}

template <typename _ValueT,
		  std::size_t _S>
inline _ValueT* any_cast(static_any<_S>* a)
{
	if (!a->template has<_ValueT>())
		return nullptr;

	return a->template as<_ValueT>();
}

template <typename _ValueT,
		  std::size_t _S>
inline const _ValueT* any_cast(const static_any<_S>* a)
{
	return any_cast<const _ValueT>(const_cast<static_any<_S>*>(a));
}

template <typename _ValueT,
		  std::size_t _S>
inline _ValueT& any_cast(static_any<_S>& a)
{
	if (!a.template has<_ValueT>())
		throw bad_any_cast(a.type(), typeid(_ValueT));

	return *a.template as<_ValueT>();
}

template <typename _ValueT,
		  std::size_t _S>
inline const _ValueT& any_cast(const static_any<_S>& a)
{
	return any_cast<const _ValueT>(const_cast<static_any<_S>&>(a));
}

template <std::size_t _S>
template <typename _T>
const _T& static_any<_S>::get() const
{
	return any_cast<_T>(*this);
}

template <std::size_t _S>
template <typename _T>
_T& static_any<_S>::get()
{
	return any_cast<_T>(*this);
}


template <std::size_t _N>
struct static_any_t
{
	typedef std::size_t size_type;
	typedef std::aligned_storage_t<_N> storage_t;

	static constexpr size_type capacity() { return _N; }

	static_any_t() = default;
	static_any_t(const static_any_t&) = default;

	template <typename _ValueT>
	static_any_t(_ValueT&& t)
	{
		copy(std::forward<_ValueT>(t));
	}

	template <typename _ValueT>
	static_any_t& operator=(_ValueT&& t)
	{
		copy(std::forward<_ValueT>(t));
		return *this;
	}

	template <typename _ValueT>
	_ValueT& get() { return *reinterpret_cast<_ValueT*>(&buff_); }

	template <typename _ValueT>
	const _ValueT& get() const { return *reinterpret_cast<const _ValueT*>(&buff_); }

private:
	template <typename _ValueT>
	void copy(_ValueT&& t)
	{
		using NonConstT = std::remove_cv_t<std::remove_reference_t<_ValueT>>;

#if __GNUG__ && __GNUC__ < 5
		static_assert(std::has_trivial_copy_constructor<NonConstT>::value, "_ValueT is not trivially copyable");
#else
		static_assert(std::is_trivially_copyable<NonConstT>::value, "_ValueT is not trivially copyable");
#endif

		static_assert(capacity() >= sizeof(_ValueT), "_ValueT is too big to be copied to static_any");

		std::memcpy(&buff_, reinterpret_cast<char*>(&t), sizeof(_ValueT));
	}

	storage_t buff_;
};
