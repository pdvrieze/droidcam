//
// Created by pdvrieze on 07/04/2020.
//

#include "util.h"
#include "common.h"

const char *DroidcamException::what() const noexcept
{
	return msg.data();
}

DroidcamException::DroidcamException(const std::string_view what_arg)
{
	msg = what_arg;
}
