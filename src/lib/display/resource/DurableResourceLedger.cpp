// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "DurableResourceLedger.h"
#include "display/control/InfrastructureControl.h"
#include "display/core/InfrastructureTypes.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryFile>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <limits>
#include <set>
#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace deskflow::display::infra {
namespace {
constexpr quint64 maximumLog = 4 * 1024 * 1024;
constexpr qsizetype headerBytes = 32;
bool id(const QString& s,size_t bytes) { return validId(s.toStdString(),bytes); }
bool bounded(const QString& s,qsizetype maximum) {
  if(s.isEmpty()||s.size()>maximum)return false;
  for(auto c:s)if(c.unicode()<32||c.unicode()==127)return false;
  return true;
}
QString phaseName(ResourcePhase p) {
  switch(p){case ResourcePhase::Intent:return "intent";case ResourcePhase::Observed:return "observed";
    case ResourcePhase::Active:return "active";case ResourcePhase::Releasing:return "releasing";
    case ResourcePhase::CleanupRequired:return "cleanupRequired";case ResourcePhase::ReleaseVerified:return "releaseVerified";
    case ResourcePhase::Closed:return "closed";}
  return {};
}
std::optional<ResourcePhase> phase(const QString& s) {
  for(auto p:{ResourcePhase::Intent,ResourcePhase::Observed,ResourcePhase::Active,ResourcePhase::Releasing,
      ResourcePhase::CleanupRequired,ResourcePhase::ReleaseVerified,ResourcePhase::Closed})if(phaseName(p)==s)return p;
  return {};
}
bool allowed(ResourcePhase from,ResourcePhase to) {
  switch(from){
  case ResourcePhase::Intent:return to==ResourcePhase::Observed||to==ResourcePhase::Releasing||to==ResourcePhase::CleanupRequired;
  case ResourcePhase::Observed:return to==ResourcePhase::Active||to==ResourcePhase::Releasing||to==ResourcePhase::CleanupRequired;
  case ResourcePhase::Active:return to==ResourcePhase::Releasing||to==ResourcePhase::CleanupRequired;
  case ResourcePhase::Releasing:return to==ResourcePhase::ReleaseVerified||to==ResourcePhase::CleanupRequired;
  case ResourcePhase::CleanupRequired:return to==ResourcePhase::Releasing;
  case ResourcePhase::ReleaseVerified:return to==ResourcePhase::Closed;
  case ResourcePhase::Closed:return false;
  }return false;
}
bool sameKeys(const QJsonObject& o,std::initializer_list<const char*> list) {
  if(o.size()!=static_cast<qsizetype>(list.size()))return false;
  for(const auto* k:list)if(!o.contains(QLatin1String(k)))return false;
  return true;
}
bool durableFd(qintptr descriptor) {
#ifdef Q_OS_WIN
  const auto handle=reinterpret_cast<HANDLE>(_get_osfhandle(static_cast<int>(descriptor)));
  return handle!=INVALID_HANDLE_VALUE&&FlushFileBuffers(handle);
#else
  if(::fsync(static_cast<int>(descriptor))!=0)return false;
#ifdef Q_OS_MACOS
  if(::fcntl(static_cast<int>(descriptor),F_FULLFSYNC)!=0)return false;
#endif
  return true;
#endif
}
bool durableDirectory(const QString& path) {
#ifdef Q_OS_WIN
  // Atomic replacements below use MOVEFILE_WRITE_THROUGH on Windows.
  Q_UNUSED(path);return true;
#else
  const auto bytes=QFile::encodeName(path);
  const int fd=::open(bytes.constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
  if(fd<0)return false;const bool ok=::fsync(fd)==0;::close(fd);return ok;
#endif
}
bool secureDirectory(const QString& path) {
  if(!QDir::isAbsolutePath(path)||QFileInfo(path).isSymLink())return false;
  if(!QDir().mkpath(path))return false;
#ifdef Q_OS_WIN
  HANDLE token=nullptr;
  if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
  DWORD bytes=0;GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
  QByteArray data(static_cast<qsizetype>(bytes),Qt::Uninitialized);
  if(!GetTokenInformation(token,TokenUser,data.data(),bytes,&bytes)){CloseHandle(token);return false;}
  CloseHandle(token);const auto user=reinterpret_cast<TOKEN_USER*>(data.data());
  PSID owner=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
  auto* target=const_cast<wchar_t*>(reinterpret_cast<const wchar_t*>(path.utf16()));
  if(GetNamedSecurityInfoW(target,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&descriptor)!=ERROR_SUCCESS)return false;
  const bool own=EqualSid(owner,user->User.Sid);LocalFree(descriptor);if(!own)return false;
  EXPLICIT_ACCESSW access{};access.grfAccessPermissions=GENERIC_ALL;access.grfAccessMode=SET_ACCESS;
  access.grfInheritance=SUB_CONTAINERS_AND_OBJECTS_INHERIT;access.Trustee.TrusteeForm=TRUSTEE_IS_SID;
  access.Trustee.TrusteeType=TRUSTEE_IS_USER;access.Trustee.ptstrName=reinterpret_cast<LPWSTR>(user->User.Sid);
  PACL acl=nullptr;if(SetEntriesInAclW(1,&access,nullptr,&acl)!=ERROR_SUCCESS)return false;
  const auto result=SetNamedSecurityInfoW(target,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,acl,nullptr);
  LocalFree(acl);return result==ERROR_SUCCESS;
#else
  struct stat st{};const auto bytes=QFile::encodeName(path);
  return ::lstat(bytes.constData(),&st)==0&&S_ISDIR(st.st_mode)&&st.st_uid==::geteuid()&&
    QFile::setPermissions(path,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
#endif
}
bool atomicWrite(const QString& path,const QByteArray& data) {
  if(QFileInfo(path).isSymLink())return false;
#ifdef Q_OS_WIN
  QTemporaryFile temp(QFileInfo(path).absolutePath()+"/ledger-XXXXXX");temp.setAutoRemove(true);
  if(!temp.open()||!temp.setPermissions(QFile::ReadOwner|QFile::WriteOwner)||temp.write(data)!=data.size()||
      !temp.flush()||!durableFd(temp.handle()))return false;
  const auto name=temp.fileName();temp.close();
  return MoveFileExW(reinterpret_cast<LPCWSTR>(name.utf16()),reinterpret_cast<LPCWSTR>(path.utf16()),
    MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
#else
  QSaveFile file(path);file.setDirectWriteFallback(false);
  if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(QFile::ReadOwner|QFile::WriteOwner)||
      file.write(data)!=data.size()||!file.flush()||!durableFd(file.handle())||!file.commit())return false;
  return durableDirectory(QFileInfo(path).absolutePath());
#endif
}
std::optional<QByteArray> readFile(const QString& path,quint64 maximum) {
  if(QFileInfo(path).isSymLink())return {};
  QFile f(path);if(!f.open(QIODevice::ReadOnly)||f.size()<0||static_cast<quint64>(f.size())>maximum)return {};
  return f.readAll();
}
quint32 crc32(const QByteArray& data) {
  quint32 value=0xffffffffU;
  for(unsigned char byte:data){value^=byte;for(unsigned b=0;b<8;++b)value=(value>>1)^((value&1)?0xedb88320U:0);}
  return value^0xffffffffU;
}
QByteArray frame(const QByteArray& payload,quint64 sequence) {
  QByteArray h(headerBytes,'\0');std::copy_n("BMDL",4,h.data());
  qToBigEndian<quint16>(1,h.data()+4);qToBigEndian<quint16>(headerBytes,h.data()+6);
  qToBigEndian<quint32>(static_cast<quint32>(payload.size()),h.data()+8);qToBigEndian(sequence,h.data()+12);
  const auto checksum=crc32(h+payload);qToBigEndian(checksum,h.data()+20);return h+payload;
}
std::optional<ResourceRecord> decodeRecord(const QJsonObject& o) {
  if(!sameKeys(o,{"recordType","schemaVersion","resourceId","resourceOwnerId","creationIntentId","sessionId",
    "planHash","recordSequence","localUserSessionRef","bootInstanceRef","providerId","providerVersion",
    "resourceKind","lifecycle","platformResourceRef","dependencyResourceIds","evidence"})||
    o.value("recordType")!="ResourceRecord"||o.value("schemaVersion")!=QJsonValue(1))return {};
  for(auto i=o.begin();i!=o.end();++i)
    if(i.key()!="schemaVersion"&&i.key()!="platformResourceRef"&&i.key()!="dependencyResourceIds"&&i.key()!="evidence"&&!i.value().isString())return {};
  const auto seq=parseCounter(o.value("recordSequence").toString().toStdString());
  const auto state=phase(o.value("lifecycle").toString());
  if(!seq||!state||!o.value("dependencyResourceIds").isArray()||!o.value("evidence").isObject()||
     (!o.value("platformResourceRef").isNull()&&!o.value("platformResourceRef").isString()))return {};
  const auto e=o.value("evidence").toObject();if(!sameKeys(e,{"kind","reference","status"}))return {};
  for(auto i=e.begin();i!=e.end();++i)if(!i.value().isString())return {};
  ResourceRecord r;
  r.id=o.value("resourceId").toString();r.owner=o.value("resourceOwnerId").toString();r.creationIntent=o.value("creationIntentId").toString();
  r.session=o.value("sessionId").toString();r.planHash=o.value("planHash").toString();r.sequence=*seq;
  r.userSession=o.value("localUserSessionRef").toString();r.bootInstance=o.value("bootInstanceRef").toString();
  r.provider=o.value("providerId").toString();r.providerVersion=o.value("providerVersion").toString();r.kind=o.value("resourceKind").toString();
  r.phase=*state;r.platformReference=o.value("platformResourceRef").toString();
  for(auto d:o.value("dependencyResourceIds").toArray()){if(!d.isString())return {};r.dependencies.push_back(d.toString());}
  r.evidence={e.value("kind").toString(),e.value("reference").toString(),e.value("status").toString()};return r;
}
}
QJsonObject DurableResourceLedger::encodeRecord(const ResourceRecord& r) {
  QJsonArray dependencies;for(const auto& d:r.dependencies)dependencies.append(d);
  return {{"recordType","ResourceRecord"},{"schemaVersion",1},{"resourceId",r.id},{"resourceOwnerId",r.owner},
    {"creationIntentId",r.creationIntent},{"sessionId",r.session},{"planHash",r.planHash},{"recordSequence",QString::number(r.sequence)},
    {"localUserSessionRef",r.userSession},{"bootInstanceRef",r.bootInstance},{"providerId",r.provider},
    {"providerVersion",r.providerVersion},{"resourceKind",r.kind},{"lifecycle",phaseName(r.phase)},
    {"platformResourceRef",r.platformReference.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(r.platformReference)},
    {"dependencyResourceIds",dependencies},{"evidence",QJsonObject{{"kind",r.evidence.kind},{"reference",r.evidence.reference},{"status",r.evidence.status}}}};
}
bool DurableResourceLedger::reject(QString error,Status status) { m_error=std::move(error);m_status=status;return false; }
DurableResourceLedger::DurableResourceLedger(QString directory,quint64 limit)
  :m_directory(std::move(directory)),m_logLimit(limit) {
  if(limit<2048||limit>maximumLog||!secureDirectory(m_directory)){m_error="LedgerUnavailable";return;}
  m_lock=std::make_unique<QLockFile>(m_directory+"/owner.lock");m_lock->setStaleLockTime(0);
  if(!m_lock->tryLock(0)){m_status=Status::Busy;m_error="LedgerBusy";return;}
  m_status=Status::Ready;
  if(QFileInfo::exists(m_directory+"/CURRENT"))load();
  else if(!QDir(m_directory).entryList({"gen-*"},QDir::Dirs|QDir::NoDotAndDotDot).isEmpty())
    reject("LedgerCurrentMissing",Status::Corrupt);
  else checkpoint();
}
QString DurableResourceLedger::activeOwner() const {
  for(const auto& r:m_records)if(r.phase!=ResourcePhase::Closed)return r.owner;return {};
}
bool DurableResourceLedger::valid(const ResourceRecord& r) const {
  if(!id(r.id,16)||!id(r.owner,16)||!id(r.creationIntent,16)||!id(r.session,16)||!id(r.planHash,32)||
     !r.sequence||!bounded(r.userSession,128)||!bounded(r.bootInstance,128)||!validName(r.provider.toStdString())||
     !bounded(r.providerVersion,64)||r.platformReference.size()>512||r.platformReference.contains(QChar::Null)||
     r.dependencies.size()>16)return false;
  const QSet<QString> kinds{"virtualOutput","capture","encoder","decoder","presenter","sharedMemory","worker"};
  const QSet<QString> evidenceKinds{"nativeObjectOwnership","driverOwnerBinding","verifiedHelperRegistry","systemAbsenceCheck","unproven"};
  if(!kinds.contains(r.kind)||!evidenceKinds.contains(r.evidence.kind)||!bounded(r.evidence.reference,1024)||
     (r.evidence.status!="verified"&&r.evidence.status!="unknown"&&r.evidence.status!="failed"))return false;
  QSet<QString> dependencies;for(const auto& d:r.dependencies)if(!id(d,16)||d==r.id||dependencies.contains(d))return false;else dependencies.insert(d);
  if(r.evidence.status=="verified"&&r.evidence.kind=="unproven")return false;
  if((r.phase==ResourcePhase::ReleaseVerified||r.phase==ResourcePhase::Closed)&&r.evidence.status!="verified")return false;
  if(r.phase==ResourcePhase::Intent&&(!r.platformReference.isEmpty()||r.evidence.status!="unknown"))return false;
  if((r.phase==ResourcePhase::Observed||r.phase==ResourcePhase::Active)&&r.platformReference.isEmpty())return false;
  return true;
}
bool DurableResourceLedger::apply(const ResourceRecord& r,bool replay) {
  if(!valid(r))return false;
  const auto old=m_records.constFind(r.id);
  if(old==m_records.cend()) {
    if(r.phase!=ResourcePhase::Intent||(!activeOwner().isEmpty()&&activeOwner()!=r.owner))return false;
    unsigned active=0;for(const auto& item:m_records)if(item.phase!=ResourcePhase::Closed)++active;
    if(active>=64)return false;
    for(const auto& item:m_records) {
      if(item.creationIntent==r.creationIntent)return false;
      if(item.owner==r.owner&&(activeOwner().isEmpty()||item.session!=r.session||item.planHash!=r.planHash||
         item.userSession!=r.userSession||item.bootInstance!=r.bootInstance))return false;
    }
    for(const auto& d:r.dependencies){const auto i=m_records.constFind(d);if(i==m_records.cend()||i->owner!=r.owner||i->phase==ResourcePhase::Closed)return false;}
  } else {
    if(!allowed(old->phase,r.phase)||old->owner!=r.owner||old->creationIntent!=r.creationIntent||old->session!=r.session||
       old->planHash!=r.planHash||old->userSession!=r.userSession||old->bootInstance!=r.bootInstance||
       old->provider!=r.provider||old->providerVersion!=r.providerVersion||old->kind!=r.kind||old->dependencies!=r.dependencies)return false;
    if(!old->platformReference.isEmpty()&&old->platformReference!=r.platformReference)return false;
    if(r.phase==ResourcePhase::ReleaseVerified||r.phase==ResourcePhase::Closed) {
      for(const auto& item:m_records)if(item.phase!=ResourcePhase::Closed&&item.dependencies.contains(r.id))return false;
    }
  }
  if(replay)m_records.insert(r.id,r);
  return true;
}
bool DurableResourceLedger::createIntent(ResourceRecord r) {
  if(m_status!=Status::Ready||m_records.contains(r.id))return false;
  if(m_sequence==std::numeric_limits<quint64>::max())return reject("LedgerSequenceExhausted",Status::Unavailable);
  r.phase=ResourcePhase::Intent;r.platformReference.clear();r.sequence=m_sequence+1;
  if(!apply(r,false))return false;return append(std::move(r));
}
bool DurableResourceLedger::transition(const QString& resource,ResourcePhase target,QString reference,ResourceEvidence evidence) {
  if(m_status!=Status::Ready||!m_records.contains(resource))return false;
  if(m_sequence==std::numeric_limits<quint64>::max())return reject("LedgerSequenceExhausted",Status::Unavailable);
  auto r=m_records.value(resource);
  if(r.phase==target)return r.platformReference==reference&&r.evidence==evidence;
  r.phase=target;r.platformReference=std::move(reference);r.evidence=std::move(evidence);r.sequence=m_sequence+1;
  if(!apply(r,false))return false;return append(std::move(r));
}
bool DurableResourceLedger::append(ResourceRecord r) {
  if(m_sequence==std::numeric_limits<quint64>::max())return reject("LedgerSequenceExhausted",Status::Unavailable);
  if(m_records.size()>=192) {if(!compact())return false;}
  r.sequence=m_sequence+1;
  const auto payload=QJsonDocument(QJsonObject{{"type","ResourceUpdate"},{"record",encodeRecord(r)}}).toJson(QJsonDocument::Compact);
  if(payload.size()>65536)return false;
  const auto encoded=frame(payload,r.sequence);
  auto path=m_directory+"/"+m_generation+"/events.log";
  if(QFileInfo(path).size()+encoded.size()>static_cast<qint64>(m_logLimit)) {
    if(!compact())return false;path=m_directory+"/"+m_generation+"/events.log";
  }
  if(!QFileInfo(path).isFile()||QFileInfo(path).isSymLink())return reject("LedgerLogUnavailable",Status::Corrupt);
  QFile file(path);
  if(!file.open(QIODevice::WriteOnly|QIODevice::Append)||file.write(encoded)!=encoded.size()||!file.flush()||!durableFd(file.handle()))
    return reject("DurabilityFailed",Status::Unavailable);
  m_records.insert(r.id,r);m_sequence=r.sequence;return true;
}
void DurableResourceLedger::pruneClosed() {
  std::vector<ResourceRecord> closed;for(const auto& r:m_records)if(r.phase==ResourcePhase::Closed)closed.push_back(r);
  std::sort(closed.begin(),closed.end(),[](const auto& a,const auto& b){return a.sequence>b.sequence;});
  for(size_t i=128;i<closed.size();++i)m_records.remove(closed[i].id);
}
bool DurableResourceLedger::compact() {
  if(m_status!=Status::Ready)return false;pruneClosed();return checkpoint();
}
bool DurableResourceLedger::checkpoint() {
  const auto generation="gen-"+QUuid::createUuid().toString(QUuid::Id128);
  const auto folder=m_directory+"/"+generation;
  if(!secureDirectory(folder))return reject("CheckpointDirectoryFailed",Status::Unavailable);
  QJsonArray resources;for(const auto& r:m_records)resources.append(encodeRecord(r));
  const auto snapshot=QJsonDocument(QJsonObject{{"schemaVersion",1},{"lastSequence",QString::number(m_sequence)},
    {"resources",resources}}).toJson(QJsonDocument::Compact);
  if(snapshot.size()>static_cast<qint64>(maximumLog)||!atomicWrite(folder+"/snapshot.json",snapshot)||
      !atomicWrite(folder+"/events.log",{})||!durableDirectory(folder))return reject("CheckpointWriteFailed",Status::Unavailable);
  const auto digest=QString::fromLatin1(QCryptographicHash::hash(snapshot,QCryptographicHash::Sha256).toHex());
  const auto current=QJsonDocument(QJsonObject{{"schemaVersion",1},{"generation",generation},{"snapshotDigest",digest}}).toJson(QJsonDocument::Compact);
  if(!atomicWrite(m_directory+"/CURRENT",current))return reject("CheckpointCommitFailed",Status::Unavailable);
  const auto previous=m_generation;m_generation=generation;
  // CURRENT is durable before old generations can be retired. Only remove
  // private, structurally recognized ledger directories; never follow links.
  for(const auto& name:QDir(m_directory).entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {
    if(name==generation||name==previous||!name.startsWith("gen-")||!id(name.mid(4),16))continue;
    const auto old=m_directory+"/"+name;
    if(QFileInfo(old).isSymLink())continue;
    const auto files=QDir(old).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System);
    if(files.size()!=2||!files.contains("snapshot.json")||!files.contains("events.log")||
        QFileInfo(old+"/snapshot.json").isSymLink()||QFileInfo(old+"/events.log").isSymLink())continue;
    QDir(old).removeRecursively();
  }
  return true;
}
bool DurableResourceLedger::load() {
  ControlError error;const auto bytes=readFile(m_directory+"/CURRENT",1024);
  if(!bytes)return reject("LedgerCurrentUnreadable",Status::Corrupt);
  const auto current=parseControlObject(*bytes,error);
  if(!current||!sameKeys(*current,{"schemaVersion","generation","snapshotDigest"}))return reject("LedgerCurrentCorrupt",Status::Corrupt);
  if(current->value("schemaVersion")!=QJsonValue(1))return reject("UnsupportedLedgerVersion",Status::UnsupportedVersion);
  const auto name=current->value("generation").toString(),digest=current->value("snapshotDigest").toString();
  if(!name.startsWith("gen-")||!id(name.mid(4),16)||!id(digest,32))return reject("LedgerCurrentCorrupt",Status::Corrupt);
  const auto folder=m_directory+"/"+name;
  if(QFileInfo(folder).isSymLink())return reject("LedgerGenerationSymlink",Status::Corrupt);
  const auto snapBytes=readFile(folder+"/snapshot.json",maximumLog),logBytes=readFile(folder+"/events.log",maximumLog);
  if(!snapBytes||!logBytes||QCryptographicHash::hash(*snapBytes,QCryptographicHash::Sha256).toHex()!=digest.toLatin1())return reject("LedgerSnapshotMismatch",Status::Corrupt);
  const auto snapshot=parseBoundedObject(*snapBytes,maximumLog,error);
  if(!snapshot||!sameKeys(*snapshot,{"schemaVersion","lastSequence","resources"})||!snapshot->value("resources").isArray()||
      !snapshot->value("lastSequence").isString())return reject("LedgerSnapshotCorrupt",Status::Corrupt);
  if(snapshot->value("schemaVersion")!=QJsonValue(1))return reject("UnsupportedLedgerVersion",Status::UnsupportedVersion);
  const auto sequenceText=snapshot->value("lastSequence").toString();
  const auto sequence=sequenceText=="0"?std::optional<uint64_t>(0):parseCounter(sequenceText.toStdString());
  if(!sequence||snapshot->value("resources").toArray().size()>192)return reject("LedgerSnapshotCorrupt",Status::Corrupt);
  m_sequence=*sequence;
  QString owner;
  for(const auto& v:snapshot->value("resources").toArray()) {
    if(!v.isObject())return reject("LedgerResourceCorrupt",Status::Corrupt);
    const auto r=decodeRecord(v.toObject());
    if(!r||!valid(*r)||r->sequence>m_sequence||m_records.contains(r->id))return reject("LedgerResourceCorrupt",Status::Corrupt);
    if(r->phase!=ResourcePhase::Closed){if(!owner.isEmpty()&&owner!=r->owner)return reject("LedgerOwnerConflict",Status::Corrupt);owner=r->owner;}
    m_records.insert(r->id,*r);
  }
  unsigned activeCount=0,closedCount=0;
  for(const auto& r:m_records) {
    if(r.phase==ResourcePhase::Closed)++closedCount;else ++activeCount;
    for(const auto& dependency:r.dependencies) {
      const auto parent=m_records.constFind(dependency);
      if(r.phase!=ResourcePhase::Closed&&(parent==m_records.cend()||parent->owner!=r.owner||
         parent->phase==ResourcePhase::Closed))return reject("LedgerDependencyCorrupt",Status::Corrupt);
    }
  }
  if(activeCount>64||closedCount>128)return reject("LedgerResourceBudgetExceeded",Status::Corrupt);
  qsizetype offset=0;
  while(offset<logBytes->size()) {
    if(logBytes->size()-offset<headerBytes)break;
    auto h=logBytes->mid(offset,headerBytes);
    if(h.first(4)!="BMDL"||qFromBigEndian<quint16>(h.constData()+6)!=headerBytes||h.mid(24)!=QByteArray(8,'\0'))return reject("LedgerHeaderCorrupt",Status::Corrupt);
    if(qFromBigEndian<quint16>(h.constData()+4)!=1)return reject("UnsupportedLedgerVersion",Status::UnsupportedVersion);
    const auto length=qFromBigEndian<quint32>(h.constData()+8);
    const auto seq=qFromBigEndian<quint64>(h.constData()+12);
    const auto crc=qFromBigEndian<quint32>(h.constData()+20);
    if(length>65536||!length||m_sequence==std::numeric_limits<quint64>::max()||seq!=m_sequence+1)return reject("LedgerSequenceCorrupt",Status::Corrupt);
    if(logBytes->size()-offset-headerBytes<length)break;
    const auto payload=logBytes->mid(offset+headerBytes,length);qToBigEndian<quint32>(0,h.data()+20);
    if(crc32(h+payload)!=crc)return reject("LedgerChecksumMismatch",Status::Corrupt);
    const auto event=parseControlObject(payload,error);
    if(!event||!sameKeys(*event,{"type","record"})||event->value("type")!="ResourceUpdate"||!event->value("record").isObject())return reject("LedgerEventCorrupt",Status::Corrupt);
    const auto record=decodeRecord(event->value("record").toObject());
    if(!record||record->sequence!=seq||!apply(*record,true))return reject("LedgerTransitionCorrupt",Status::Corrupt);
    m_sequence=seq;offset+=headerBytes+length;
  }
  if(offset!=logBytes->size()) {
    QFile file(folder+"/events.log");
    if(!file.open(QIODevice::ReadWrite)||!file.resize(offset)||!file.flush()||!durableFd(file.handle()))return reject("LedgerTailRepairFailed",Status::Unavailable);
  }
  m_generation=name;return true;
}
} // namespace deskflow::display::infra
