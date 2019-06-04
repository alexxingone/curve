/*
 * Project: curve
 * File Created: Monday, 18th February 2019 11:20:01 am
 * Author: tongguangxun
 * Copyright (c)￼ 2018 netease
 */

#include <brpc/server.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <thread>   // NOLINT
#include <mutex>    // NOLINT
#include "src/client/libcurve_file.h"
#include "src/client/client_common.h"
#include "src/client/client_config.h"
#include "include/client/libcurve.h"
#include "src/client/file_instance.h"
#include "include/curve_compiler_specific.h"
#include "src/client/iomanager4file.h"
#include "src/client/service_helper.h"

using curve::client::UserInfo;
using curve::common::ReadLockGuard;
using curve::common::WriteLockGuard;

bool globalclientinited_ = false;
curve::client::FileClient* globalclient = nullptr;

namespace curve {
namespace client {
FileClient::FileClient(): fdcount_(0) {
    inited_ = false;
    mdsClient_ = nullptr;
    fileserviceMap_.clear();
}

int FileClient::Init(const std::string& configpath) {
    if (inited_) {
        LOG(WARNING) << "already inited!";
        return 0;
    }

    if (-1 == clientconfig_.Init(configpath.c_str())) {
        LOG(ERROR) << "config init failed!";
        return -LIBCURVE_ERROR::FAILED;
    }

    if (mdsClient_ == nullptr) {
        mdsClient_ = new (std::nothrow) MDSClient();
        if (mdsClient_ == nullptr) {
            return -LIBCURVE_ERROR::FAILED;
        }

        if (LIBCURVE_ERROR::OK != mdsClient_->Initialize(
            clientconfig_.GetFileServiceOption().metaServerOpt)) {
            LOG(ERROR) << "Init global mds client failed!";
            return -LIBCURVE_ERROR::FAILED;
        }
    }

    // 在一个进程里只允许启动一次
    uint16_t dummyServerStartPort = clientconfig_.GetDummyserverStartPort();
    static std::once_flag flag;
    std::call_once(flag, [&](){
        // 启动dummy server
        int rc = -1;
        while (rc < 0) {
            rc = brpc::StartDummyServerAt(dummyServerStartPort);
            dummyServerStartPort++;
        }
    });

    google::SetLogDestination(google::INFO,
        clientconfig_.GetFileServiceOption().loginfo.logpath.c_str());

    FLAGS_minloglevel = clientconfig_.GetFileServiceOption().loginfo.loglevel;

    std::string processname("libcurve");
    google::InitGoogleLogging(processname.c_str());

    inited_ = true;
    return LIBCURVE_ERROR::OK;
}

void FileClient::UnInit() {
    if (!inited_) {
        LOG(WARNING) << "not inited!";
        return;
    }
    WriteLockGuard lk(rwlock_);
    for (auto iter : fileserviceMap_) {
        iter.second->UnInitialize();
        delete iter.second;
    }
    fileserviceMap_.clear();

    if (mdsClient_ != nullptr) {
        mdsClient_->UnInitialize();
        delete mdsClient_;
        mdsClient_ = nullptr;
    }
    google::ShutdownGoogleLogging();
    inited_ = false;
}

int FileClient::Open(const std::string& filename, const UserInfo_t& userinfo) {
    FileInstance* fileserv = new (std::nothrow) FileInstance();
    if (fileserv == nullptr ||
        !fileserv->Initialize(filename,
                              mdsClient_,
                              userinfo,
                              clientconfig_.GetFileServiceOption())) {
        LOG(ERROR) << "FileInstance initialize failed!";
        delete fileserv;
        return -1;
    }

    int ret = fileserv->Open(filename, userinfo);
    if (ret != LIBCURVE_ERROR::OK) {
        fileserv->UnInitialize();
        delete fileserv;
        return ret;
    }

    int fd = fdcount_.fetch_add(1, std::memory_order_acq_rel);

    {
        WriteLockGuard lk(rwlock_);
        fileserviceMap_[fd] = fileserv;
    }
    return fd;
}

int FileClient::Create(const std::string& filename,
                       const UserInfo_t& userinfo,
                       size_t size) {
    LIBCURVE_ERROR ret;
    if (mdsClient_ != nullptr) {
        ret = mdsClient_->CreateFile(filename, userinfo, size);
    } else {
        LOG(ERROR) << "global mds client not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }
    return -ret;
}

int FileClient::Read(int fd, char* buf, off_t offset, size_t len) {
    // 长度为0，直接返回，不做任何操作
    if (len == 0) {
        return -LIBCURVE_ERROR::OK;
    }

    if (CheckAligned(offset, len) == false) {
        return -LIBCURVE_ERROR::NOT_ALIGNED;
    }

    ReadLockGuard lk(rwlock_);
    if (CURVE_UNLIKELY(fileserviceMap_.find(fd) == fileserviceMap_.end())) {
        LOG(ERROR) << "invalid fd!";
        return -LIBCURVE_ERROR::BAD_FD;
    }

    return fileserviceMap_[fd]->Read(buf, offset, len);
}

int FileClient::Write(int fd, const char* buf, off_t offset, size_t len) {
    // 长度为0，直接返回，不做任何操作
    if (len == 0) {
        return -LIBCURVE_ERROR::OK;
    }

    if (CheckAligned(offset, len) == false) {
        return -LIBCURVE_ERROR::NOT_ALIGNED;
    }

    ReadLockGuard lk(rwlock_);
    if (CURVE_UNLIKELY(fileserviceMap_.find(fd) == fileserviceMap_.end())) {
        LOG(ERROR) << "invalid fd!";
        return -LIBCURVE_ERROR::BAD_FD;
    }

    return fileserviceMap_[fd]->Write(buf, offset, len);
}

int FileClient::AioRead(int fd, CurveAioContext* aioctx) {
    // 长度为0，直接返回，不做任何操作
    if (aioctx->length == 0) {
        return -LIBCURVE_ERROR::OK;
    }

    if (CheckAligned(aioctx->offset, aioctx->length) == false) {
        return -LIBCURVE_ERROR::NOT_ALIGNED;
    }

    int ret = -LIBCURVE_ERROR::FAILED;
    ReadLockGuard lk(rwlock_);
    if (CURVE_UNLIKELY(fileserviceMap_.find(fd) == fileserviceMap_.end())) {
        LOG(ERROR) << "invalid fd!";
        ret = -LIBCURVE_ERROR::BAD_FD;
    } else {
        ret = fileserviceMap_[fd]->AioRead(aioctx);
    }

    return ret;
}

int FileClient::AioWrite(int fd, CurveAioContext* aioctx) {
    // 长度为0，直接返回，不做任何操作
    if (aioctx->length == 0) {
        return -LIBCURVE_ERROR::OK;
    }

    if (CheckAligned(aioctx->offset, aioctx->length) == false) {
        return -LIBCURVE_ERROR::NOT_ALIGNED;
    }

    int ret = -LIBCURVE_ERROR::FAILED;
    ReadLockGuard lk(rwlock_);
    if (CURVE_UNLIKELY(fileserviceMap_.find(fd) == fileserviceMap_.end())) {
        LOG(ERROR) << "invalid fd!";
        ret = -LIBCURVE_ERROR::BAD_FD;
    } else {
        ret = fileserviceMap_[fd]->AioWrite(aioctx);
    }

    return ret;
}

int FileClient::Rename(const UserInfo_t& userinfo,
                                  const std::string& oldpath,
                                  const std::string& newpath) {
    LIBCURVE_ERROR ret;
    if (mdsClient_ != nullptr) {
        ret = mdsClient_->RenameFile(userinfo, oldpath, newpath);
    } else {
        LOG(ERROR) << "global mds client not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }
    return -ret;
}

int FileClient::Extend(const std::string& filename,
                                  const UserInfo_t& userinfo,
                                  uint64_t newsize) {
    LIBCURVE_ERROR ret;
    if (mdsClient_ != nullptr) {
        ret = mdsClient_->Extend(filename, userinfo, newsize);
    } else {
        LOG(ERROR) << "global mds client not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }
    return -ret;
}

int FileClient::Unlink(const std::string& filename,
                       const UserInfo_t& userinfo,
                       bool deleteforce) {
    LIBCURVE_ERROR ret;
    if (mdsClient_ != nullptr) {
        ret = mdsClient_->DeleteFile(filename, userinfo, deleteforce);
    } else {
        LOG(ERROR) << "global mds client not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }
    return -ret;
}

int FileClient::StatFile(const std::string& filename,
                         const UserInfo_t& userinfo,
                         FileStatInfo* finfo) {
    FInfo_t fi;
    int ret;
    if (mdsClient_ != nullptr) {
        ret = mdsClient_->GetFileInfo(filename, userinfo, &fi);
    } else {
        LOG(ERROR) << "global mds client not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    if (ret == LIBCURVE_ERROR::OK) {
        finfo->id       = fi.id;
        finfo->parentid = fi.parentid;
        finfo->ctime    = fi.ctime;
        finfo->length   = fi.length;
        finfo->filetype = fi.filetype;
    }

    return -ret;
}

int FileClient::Listdir(const std::string& dirpath,
                        const UserInfo_t& userinfo,
                        std::vector<FileStatInfo>* filestatVec) {
    LIBCURVE_ERROR ret;
    if (mdsClient_ != nullptr) {
        ret = mdsClient_->Listdir(dirpath, userinfo, filestatVec);
    } else {
        LOG(ERROR) << "global mds client not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }
    return -ret;
}

int FileClient::Mkdir(const std::string& dirpath, const UserInfo_t& userinfo) {
    LIBCURVE_ERROR ret;
    if (mdsClient_ != nullptr) {
        ret = mdsClient_->CreateFile(dirpath, userinfo, 0, false);
    } else {
        LOG(ERROR) << "global mds client not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }
    return -ret;
}

int FileClient::Rmdir(const std::string& dirpath, const UserInfo_t& userinfo) {
    LIBCURVE_ERROR ret;
    if (mdsClient_ != nullptr) {
        ret = mdsClient_->DeleteFile(dirpath, userinfo);
    } else {
        LOG(ERROR) << "global mds client not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }
    return -ret;
}

int FileClient::ChangeOwner(const std::string& filename,
                            const std::string& newOwner,
                            const UserInfo_t& userinfo) {
    LIBCURVE_ERROR ret;
    if (mdsClient_ != nullptr) {
        ret = mdsClient_->ChangeOwner(filename, newOwner, userinfo);
    } else {
        LOG(ERROR) << "global mds client not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }
    return -ret;
}

int FileClient::Close(int fd) {
    WriteLockGuard lk(rwlock_);
    auto iter = fileserviceMap_.find(fd);
    if (iter == fileserviceMap_.end()) {
        LOG(ERROR) << "can not find " << fd;
        return -LIBCURVE_ERROR::FAILED;
    }
    int ret = fileserviceMap_[fd]->Close();
    if (ret == LIBCURVE_ERROR::OK) {
        fileserviceMap_[fd]->UnInitialize();
        LOG(ERROR) << "uninitialize " << fd;
        delete fileserviceMap_[fd];
        fileserviceMap_.erase(iter);
    } else {
        LOG(ERROR) << "close failed " << fd;
    }
    return ret;
}

bool FileClient::CheckAligned(off_t offset, size_t length) {
    return (offset % IO_ALIGNED_BLOCK_SIZE == 0) &&
           (length % IO_ALIGNED_BLOCK_SIZE == 0);
}

}   // namespace client
}   // namespace curve


// 全局初始化与反初始化
int GlobalInit(const char* configpath);
void GlobalUnInit();

int Init(const char* path) {
    return GlobalInit(path);
}

int Open4Qemu(const char* filename) {
    curve::client::UserInfo_t userinfo;
    std::string realname;
    if (!curve::client::ServiceHelper::GetUserInfoFromFilename(filename,
                                        &realname,
                                        &userinfo.owner)) {
        LOG(ERROR) << "get user info from filename failed!";
        return -LIBCURVE_ERROR::FAILED;
    }

    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    return globalclient->Open(realname, userinfo);
}

int Extend4Qemu(const char* filename, int64_t newsize) {
    curve::client::UserInfo_t userinfo;
    std::string realname;
    if (!curve::client::ServiceHelper::GetUserInfoFromFilename(filename,
                                        &realname,
                                        &userinfo.owner)) {
        LOG(ERROR) << "get user info from filename failed!";
        return -LIBCURVE_ERROR::FAILED;
    }
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }
    if (newsize <= 0) {
        LOG(ERROR) << "File size is wrong, " << newsize;
        return -LIBCURVE_ERROR::FAILED;
    }

    return globalclient->Extend(realname, userinfo,
            static_cast<uint64_t>(newsize));
}

int Open(const char* filename, const C_UserInfo_t* userinfo) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    return globalclient->Open(filename,
                              UserInfo(userinfo->owner, userinfo->password));
}

int Read(int fd, char* buf, off_t offset, size_t length) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    return globalclient->Read(fd, buf, offset, length);
}

int Write(int fd, const char* buf, off_t offset, size_t length) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    return globalclient->Write(fd, buf, offset, length);
}

int AioRead(int fd, CurveAioContext* aioctx) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    DVLOG(9) << "offset: " << aioctx->offset
        << " length: " << aioctx->length
        << " op: " << aioctx->op;
    return globalclient->AioRead(fd, aioctx);
}

int AioWrite(int fd, CurveAioContext* aioctx) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    DVLOG(9) << "offset: " << aioctx->offset
        << " length: " << aioctx->length
        << " op: " << aioctx->op
        << " buf: " << *(unsigned int*)aioctx->buf;
    return globalclient->AioWrite(fd, aioctx);
}

int Create(const char* filename, const C_UserInfo_t* userinfo, size_t size) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    return globalclient->Create(filename,
            UserInfo(userinfo->owner, userinfo->password), size);
}

int Rename(const C_UserInfo_t* userinfo,
           const char* oldpath, const char* newpath) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    return globalclient->Rename(UserInfo(userinfo->owner, userinfo->password),
            oldpath, newpath);
}

int Extend(const char* filename,
           const C_UserInfo_t* userinfo, uint64_t newsize) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    return globalclient->Extend(filename,
            UserInfo(userinfo->owner, userinfo->password), newsize);
}

int Unlink(const char* filename, const C_UserInfo_t* userinfo) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    return globalclient->Unlink(filename,
            UserInfo(userinfo->owner, userinfo->password));
}

int DeleteForce(const char* filename, const C_UserInfo_t* userinfo) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    return globalclient->Unlink(filename,
            UserInfo(userinfo->owner, userinfo->password),
            true);
}

DirInfo_t* OpenDir(const char* dirpath, const C_UserInfo_t* userinfo) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return nullptr;
    }

    DirInfo_t* dirinfo = new (std::nothrow) DirInfo_t;
    dirinfo->dirpath = const_cast<char*>(dirpath);
    dirinfo->userinfo = const_cast<C_UserInfo_t*>(userinfo);
    dirinfo->fileStat = nullptr;

    return dirinfo;
}

int Listdir(DirInfo_t* dirinfo) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    if (dirinfo == nullptr) {
        LOG(ERROR) << "dir not opened!";
        return -LIBCURVE_ERROR::FAILED;
    }

    std::vector<FileStatInfo> fileStat;
    int ret = globalclient->Listdir(dirinfo->dirpath,
             UserInfo(dirinfo->userinfo->owner, dirinfo->userinfo->password),
             &fileStat);

    dirinfo->dirSize = fileStat.size();
    dirinfo->fileStat = new (std::nothrow) FileStatInfo_t[dirinfo->dirSize];

    if (dirinfo->fileStat == nullptr) {
        LOG(ERROR) << "allocate FileStatInfo memory failed!";
        return -LIBCURVE_ERROR::FAILED;
    }

    for (int i = 0; i < dirinfo->dirSize; i++) {
        dirinfo->fileStat[i].id = fileStat[i].id;
        dirinfo->fileStat[i].parentid = fileStat[i].parentid;
        dirinfo->fileStat[i].filetype = fileStat[i].filetype;
        dirinfo->fileStat[i].length = fileStat[i].length;
        dirinfo->fileStat[i].ctime = fileStat[i].ctime;
        memset(dirinfo->fileStat[i].owner, 0, NAME_MAX_SIZE);
        memcpy(dirinfo->fileStat[i].owner, fileStat[i].owner, NAME_MAX_SIZE);
        memset(dirinfo->fileStat[i].filename, 0, NAME_MAX_SIZE);
        memcpy(dirinfo->fileStat[i].filename, fileStat[i].filename,
                NAME_MAX_SIZE);
    }

    return ret;
}

void CloseDir(DirInfo_t* dirinfo) {
    if (dirinfo != nullptr) {
        if (dirinfo->fileStat != nullptr) {
            delete[] dirinfo->fileStat;
        }
        delete dirinfo;
        LOG(INFO) << "close dir";
    }
}

int Mkdir(const char* dirpath, const C_UserInfo_t* userinfo) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    return globalclient->Mkdir(dirpath,
            UserInfo(userinfo->owner, userinfo->password));
}

int Rmdir(const char* dirpath, const C_UserInfo_t* userinfo) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    return globalclient->Rmdir(dirpath,
            UserInfo(userinfo->owner, userinfo->password));
}

int Close(int fd) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    return globalclient->Close(fd);
}

int StatFile4Qemu(const char* filename, FileStatInfo* finfo) {
    curve::client::UserInfo_t userinfo;
    std::string realname;
    if (!curve::client::ServiceHelper::GetUserInfoFromFilename(filename,
                                        &realname,
                                        &userinfo.owner)) {
        LOG(ERROR) << "get user info from filename failed!";
        return -LIBCURVE_ERROR::FAILED;
    }

    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    return globalclient->StatFile(realname, userinfo, finfo);
}

int StatFile(const char* filename,
             const C_UserInfo_t* cuserinfo, FileStatInfo* finfo) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    curve::client::UserInfo_t userinfo(cuserinfo->owner, cuserinfo->password);
    return globalclient->StatFile(filename, userinfo, finfo);
}

int ChangeOwner(const char* filename,
                const char* newOwner,
                const C_UserInfo_t* cuserinfo) {
    if (globalclient == nullptr) {
        LOG(ERROR) << "not inited!";
        return -LIBCURVE_ERROR::FAILED;
    }

    curve::client::UserInfo_t userinfo(cuserinfo->owner, cuserinfo->password);
    return globalclient->ChangeOwner(filename, newOwner, userinfo);
}

void UnInit() {
    GlobalUnInit();
}

int GlobalInit(const char* path) {
    int ret = 0;
    if (globalclientinited_) {
        LOG(INFO) << "global cient already inited!";
        return LIBCURVE_ERROR::OK;
    }
    if (globalclient == nullptr) {
        globalclient = new (std::nothrow) curve::client::FileClient();
        if (globalclient != nullptr) {
            ret = globalclient->Init(path);
            LOG(INFO) << "create global client instance success!";
        } else {
            LOG(ERROR) << "create global client instance fail!";
        }
    }
    globalclientinited_ = ret == 0;
    return -ret;
}

void GlobalUnInit() {
    if (globalclient != nullptr) {
        globalclient->UnInit();
        delete globalclient;
        globalclient = nullptr;
        globalclientinited_ = false;
        LOG(INFO) << "destory global client instance success!";
    }
}
