// SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eventshandler.h"
#include "dfmplugin_disk_encrypt_global.h"
#include "gui/encryptparamsinputdialog.h"
#include "gui/encryptprocessdialog.h"
#include "gui/unlockpartitiondialog.h"
#include "utils/encryptutils.h"

#include <dfm-framework/dpf.h>

#include <QApplication>
#include <QSettings>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCall>

#include <DDialog>

Q_DECLARE_METATYPE(QString *)
Q_DECLARE_METATYPE(bool *)

using namespace dfmplugin_diskenc;
using namespace disk_encrypt;
DWIDGET_USE_NAMESPACE;

EventsHandler *EventsHandler::instance()
{
    static EventsHandler ins;
    return &ins;
}

void EventsHandler::bindDaemonSignals()
{
    auto conn = [this](const char *sig, const char *slot) {
        QDBusConnection::systemBus().connect(kDaemonBusName,
                                             kDaemonBusPath,
                                             kDaemonBusIface,
                                             sig,
                                             this,
                                             slot);
    };
    conn("PrepareEncryptDiskResult", SLOT(onPreencryptResult(const QString &, const QString &, const QString &, int)));
    conn("EncryptDiskResult", SLOT(onEncryptResult(const QString &, const QString &, int)));
    conn("EncryptProgress", SLOT(onEncryptProgress(const QString &, const QString &, double)));
    conn("DecryptDiskResult", SLOT(onDecryptResult(const QString &, const QString &, const QString &, int)));
    conn("DecryptProgress", SLOT(onDecryptProgress(const QString &, const QString &, double)));
    conn("ChangePassphressResult", SLOT(onChgPassphraseResult(const QString &, const QString &, const QString &, int)));
}

void EventsHandler::hookEvents()
{
    dpfHookSequence->follow("dfmplugin_computer", "hook_Device_AcquireDevPwd",
                            this, &EventsHandler::onAcquireDevicePwd);
}

bool EventsHandler::hasEnDecryptJob()
{
    return !(encryptDialogs.isEmpty() && decryptDialogs.isEmpty());
}

void EventsHandler::onPreencryptResult(const QString &dev, const QString &devName, const QString &, int code)
{
    QApplication::restoreOverrideCursor();

    if (code != kSuccess) {
        showPreEncryptError(dev, devName, code);
        return;
    }

    qInfo() << "reboot is required..." << dev;
    showRebootOnPreencrypted(dev, devName);
}

void EventsHandler::onEncryptResult(const QString &dev, const QString &devName, int code)
{
    QApplication::restoreOverrideCursor();
    if (encryptDialogs.contains(dev)) {
        delete encryptDialogs.value(dev);
        encryptDialogs.remove(dev);
    }

    QString device = QString("%1(%2)").arg(devName).arg(dev.mid(5));

    QString title = tr("Encrypt done");
    QString msg = tr("Device %1 has been encrypted").arg(device);
    if (code != 0) {
        title = tr("Encrypt failed");
        msg = tr("Device %1 encrypt failed, please see log for more information.(%2)")
                      .arg(device)
                      .arg(code);
    }

    dialog_utils::showDialog(title, msg, code != 0 ? dialog_utils::kError : dialog_utils::kInfo);
}

void EventsHandler::onDecryptResult(const QString &dev, const QString &devName, const QString &, int code)
{
    QApplication::restoreOverrideCursor();
    if (decryptDialogs.contains(dev)) {
        decryptDialogs.value(dev)->deleteLater();
        decryptDialogs.remove(dev);
    }

    if (code == -kRebootRequired)
        showRebootOnDecrypted(dev, devName);
    else
        showDecryptError(dev, devName, code);
}

void EventsHandler::onChgPassphraseResult(const QString &dev, const QString &devName, const QString &, int code)
{
    QApplication::restoreOverrideCursor();
    showChgPwdError(dev, devName, code);
}

void EventsHandler::onEncryptProgress(const QString &dev, const QString &devName, double progress)
{
    if (!encryptDialogs.contains(dev)) {
        QString device = QString("%1(%2)").arg(devName).arg(dev.mid(5));

        QApplication::restoreOverrideCursor();
        auto dlg = new EncryptProcessDialog(tr("Encrypting...%1").arg(device));
        connect(dlg, &EncryptProcessDialog::destroyed,
                this, [this, dev] { encryptDialogs.remove(dev); });
        encryptDialogs.insert(dev, dlg);
    }
    auto dlg = encryptDialogs.value(dev);
    dlg->updateProgress(progress);
    dlg->show();
}

void EventsHandler::onDecryptProgress(const QString &dev, const QString &devName, double progress)
{
    if (!decryptDialogs.contains(dev)) {
        QString device = QString("%1(%2)").arg(devName).arg(dev.mid(5));

        QApplication::restoreOverrideCursor();
        decryptDialogs.insert(dev, new EncryptProcessDialog(tr("Decrypting...%1").arg(device)));
    }

    auto dlg = decryptDialogs.value(dev);
    dlg->updateProgress(progress);
    dlg->show();
}

bool EventsHandler::onAcquireDevicePwd(const QString &dev, QString *pwd, bool *cancelled)
{
    if (!pwd || !cancelled)
        return false;

    int type = device_utils::encKeyType(dev);
    switch (type) {
    case SecKeyType::kTPMAndPIN:
        *pwd = acquirePassphraseByPIN(dev, *cancelled);
        break;
    case SecKeyType::kTPMOnly:
        *pwd = acquirePassphraseByTPM(dev, *cancelled);
        break;
    case SecKeyType::kPasswordOnly:
        *pwd = acquirePassphrase(dev, *cancelled);
        break;
    default:
        return false;
    }

    if (pwd->isEmpty() && !*cancelled) {
        QString title;
        if (type == kTPMAndPIN)
            title = tr("Wrong PIN");
        else if (type == kPasswordOnly)
            title = tr("Wrong passphrase");
        else
            title = tr("TPM error");

        dialog_utils::showDialog(title, tr("Please use recovery key to unlock device."),
                                 dialog_utils::kInfo);
        *cancelled = true;
    }

    return true;
}

QString EventsHandler::acquirePassphrase(const QString &dev, bool &cancelled)
{
    UnlockPartitionDialog dlg(UnlockPartitionDialog::kPwd);
    int ret = dlg.exec();
    if (ret != 1) {
        cancelled = true;
        return "";
    }
    return dlg.getUnlockKey().second;
}

QString EventsHandler::acquirePassphraseByPIN(const QString &dev, bool &cancelled)
{
    UnlockPartitionDialog dlg(UnlockPartitionDialog::kPin);
    int ret = dlg.exec();
    if (ret != 1) {
        cancelled = true;
        return "";
    }
    auto keys = dlg.getUnlockKey();
    if (keys.first == UnlockPartitionDialog::kPin)
        return tpm_passphrase_utils::getPassphraseFromTPM(dev, keys.second);
    else
        return keys.second;
}

QString EventsHandler::acquirePassphraseByTPM(const QString &dev, bool &)
{
    return tpm_passphrase_utils::getPassphraseFromTPM(dev, "");
}

void EventsHandler::showPreEncryptError(const QString &dev, const QString &devName, int code)
{
    QString title;
    QString msg;
    QString device = QString("%1(%2)").arg(devName).arg(dev.mid(5));

    bool showError = false;
    switch (-code) {
    case (kSuccess):
        title = tr("Preencrypt done");
        msg = tr("Device %1 has been preencrypt, please reboot to finish encryption.")
                      .arg(device);
        break;
    case kUserCancelled:
        title = tr("Encrypt disk");
        msg = tr("User cancelled operation");
        break;
    default:
        title = tr("Preencrypt failed");
        msg = tr("Device %1 preencrypt failed, please see log for more information.(%2)")
                      .arg(device)
                      .arg(code);
        showError = true;
        break;
    }

    dialog_utils::showDialog(title, msg,
                             showError ? dialog_utils::kError : dialog_utils::kInfo);
}

void EventsHandler::showDecryptError(const QString &dev, const QString &devName, int code)
{
    QString title;
    QString msg;
    QString device = QString("%1(%2)").arg(devName).arg(dev.mid(5));

    bool showFailed = true;
    switch (-code) {
    case (kSuccess):
        title = tr("Decrypt done");
        msg = tr("Device %1 has been decrypted").arg(device);
        showFailed = false;
        break;
    case kUserCancelled:
        title = tr("Decrypt disk");
        msg = tr("User cancelled operation");
        showFailed = false;
        break;
    case kErrorWrongPassphrase:
        title = tr("Decrypt disk");
        msg = tr("Wrong passpharse or PIN");
        break;
    default:
        title = tr("Decrypt failed");
        msg = tr("Device %1 Decrypt failed, please see log for more information.(%2)")
                      .arg(device)
                      .arg(code);
        break;
    }

    dialog_utils::showDialog(title, msg,
                             showFailed ? dialog_utils::kError : dialog_utils::kInfo);
}

void EventsHandler::showChgPwdError(const QString &dev, const QString &devName, int code)
{
    QString title;
    QString msg;
    QString device = QString("%1(%2)").arg(devName).arg(dev.mid(5));
    bool showError = false;
    switch (-code) {
    case (kSuccess):
        title = tr("Change passphrase done");
        msg = tr("%1's passphrase has been changed").arg(device);
        break;
    case kUserCancelled:
        title = tr("Change passphrase");
        msg = tr("User cancelled operation");
        break;
    case kErrorChangePassphraseFailed:
        title = tr("Change passphrase failed");
        msg = tr("Wrong passpharse or PIN");
        showError = true;
        break;
    default:
        title = tr("Change passphrase failed");
        msg = tr("Device %1 change passphrase failed, please see log for more information.(%2)")
                      .arg(device)
                      .arg(code);
        showError = true;
        break;
    }

    dialog_utils::showDialog(title, msg,
                             showError ? dialog_utils::kError : dialog_utils::kInfo);
}

void EventsHandler::showRebootOnPreencrypted(const QString &device, const QString &devName)
{
    QString dev = QString("%1(%2)").arg(devName).arg(device.mid(5));

    DDialog dlg;
    dlg.setIcon(QIcon::fromTheme("dialog-information"));
    dlg.setTitle(tr("Preencrypt done"));
    dlg.setMessage(tr("Device %1 has been preencrypt, please reboot to finish encryption.")
                           .arg(dev));
    dlg.addButtons({ tr("Reboot later"), tr("Reboot now") });
    if (dlg.exec() == 1)
        requestReboot();
}

void EventsHandler::showRebootOnDecrypted(const QString &device, const QString &devName)
{
    QString dev = QString("%1(%2)").arg(devName).arg(device.mid(5));

    DDialog dlg;
    dlg.setIcon(QIcon::fromTheme("dialog-information"));
    dlg.setTitle(tr("Decrypt device"));
    dlg.setMessage(tr("Please reboot to decrypt device %1.")
                           .arg(dev));
    dlg.addButtons({ tr("Reboot later"), tr("Reboot now") });
    if (dlg.exec() == 1)
        requestReboot();
}

void EventsHandler::requestReboot()
{
    qInfo() << "reboot is confirmed...";
    QDBusInterface sessMng("com.deepin.SessionManager",
                           "/com/deepin/SessionManager",
                           "com.deepin.SessionManager");
    sessMng.asyncCall("RequestReboot");
}

EventsHandler::EventsHandler(QObject *parent)
    : QObject { parent }
{
}
