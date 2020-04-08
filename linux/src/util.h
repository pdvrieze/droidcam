//
// Created by pdvrieze on 07/04/2020.
//

#ifndef DROIDCAM_UTIL_H
#define DROIDCAM_UTIL_H

#include <functional>

template<class T, class U>
inline void doFree(T *&ptr, void (*freeFunc)(U *))
{
	freeFunc(static_cast<U *>(ptr));
	ptr = nullptr;
}

template<class T, class U>
inline void doFree(T *&ptr, std::function<void(U *)> freeFunc)
{
	freeFunc(static_cast<U *>(ptr));
	ptr = nullptr;
}

#define FREE_OBJECT(obj, free_func) { dbgprint(" Freeing(" #obj ") %p\n", obj); doFree(obj, free_func); }

class DroidcamException : std::exception {
public:
	explicit DroidcamException(const std::string_view what_arg);

	virtual const char *what() const noexcept override;

private:
	std::string msg;
};

#endif //DROIDCAM_UTIL_H
