#ifndef HANDLER_CREATOR_H
#define HANDLER_CREATOR_H
#include <memory>
#include "include/packageHandlerDeb.h"
#include "include/rcutils.h"
class handlerCreator{
public:
	handlerCreator() = default;
	virtual ~handlerCreator() = default;
	std::unique_ptr<PackageHandler> getPackageHandler(const std::string& pkg);
};
#endif
