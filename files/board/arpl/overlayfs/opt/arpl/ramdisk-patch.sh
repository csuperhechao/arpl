#!/usr/bin/env bash

. /opt/arpl/include/functions.sh
. /opt/arpl/include/addons.sh

# Sanity check
[ -f "${ORI_RDGZ_FILE}" ] || die "${ORI_RDGZ_FILE} not found!"

echo -n "Patching Ramdisk"

# Remove old rd.gz patched
rm -f "${MOD_RDGZ_FILE}"

# Unzipping ramdisk
echo -n "."
rm -rf "${RAMDISK_PATH}"  # Force clean
mkdir -p "${RAMDISK_PATH}"
(cd "${RAMDISK_PATH}"; xz -dc < "${ORI_RDGZ_FILE}" | cpio -idm) >/dev/null 2>&1

# Check if DSM buildnumber changed
. "${RAMDISK_PATH}/etc/VERSION"

MODEL="`readConfigKey "model" "${USER_CONFIG_FILE}"`"
BUILD="`readConfigKey "build" "${USER_CONFIG_FILE}"`"
LKM="`readConfigKey "lkm" "${USER_CONFIG_FILE}"`"
AUTO_BOOT_UPDATE="`readConfigKey "auto_boot_update" "${USER_CONFIG_FILE}"`"

function do_update_arpl(){

	ACTUALVERSION="v${ARPL_VERSION}"
	TAG="`curl --insecure -s https://api.github.com/repos/jimmygalland/arpl/releases/latest | grep "tag_name" | awk '{print substr($2, 2, length($2)-3)}'`"
	if [ $? -ne 0 -o -z "${TAG}" ]; then
		return 2
	fi
	if [ "${ACTUALVERSION}" = "${TAG}" ]; then
		return 2
	fi

	STATUS=`curl --insecure -s -w "%{http_code}" -L "https://github.com/jimmygalland/arpl/releases/download/${TAG}/bzImage" -o /tmp/bzImage`
	if [ $? -ne 0 -o ${STATUS} -ne 200 ]; then
		return 2
	fi
	STATUS=`curl --insecure -s -w "%{http_code}" -L "https://github.com/jimmygalland/arpl/releases/download/${TAG}/rootfs.cpio.xz" -o /tmp/rootfs.cpio.xz`
	if [ $? -ne 0 -o ${STATUS} -ne 200 ]; then
		return 2
	fi
	mv /tmp/bzImage "${ARPL_BZIMAGE_FILE}"
	mv /tmp/rootfs.cpio.xz "${ARPL_RAMDISK_FILE}"
	return 0
}

if [ ${BUILD} -ne ${buildnumber} ]; then
	echo -e "\033[A\n\033[1;32mBuild number changed from \033[1;31m${BUILD}\033[1;32m to \033[1;31m${buildnumber}\033[0m"
	echo -n "Patching Ramdisk."
	# Update new buildnumber
	OLDBUILD=${BUILD}
	BUILD=${buildnumber}
fi

echo -n "."
# Read model data
PLATFORM="`readModelKey "${MODEL}" "platform"`"
KVER="`readModelKey "${MODEL}" "builds.${BUILD}.kver"`"
RD_COMPRESSED="`readModelKey "${MODEL}" "builds.${BUILD}.rd-compressed"`"

[ -z "${PLATFORM}" ] && die "ERROR: Platform Configuration for model ${MODEL} not found."


if ( [ -z "${KVER}" ] && [ "${BUILD}" -gt "${OLDBUILD}" ] ); then

  if ( [ "${AUTO_BOOT_UPGRADE}" = "yes"] ); then
	echo  -e "\033[A\n\033[1;32mTry do auto update arpl\033[0m"

	do_update_arpl

	if [ $? -ne 2]; then
		echo  -e "\033[A\n\033[1;32mArpl update OK\033[0m"
		echo  -e "reboot in 5 sec."
		sleep 5
		reboot
	fi

   fi

   echo -e "\033[A\n\033[1;32mTry patch ramdisk with latest platform configuration\033[0m" 
   BUILD=${OLDBUILD}
   KVER="`readModelKey "${MODEL}" "builds.${BUILD}.kver"`"
   RD_COMPRESSED="`readModelKey "${MODEL}" "builds.${BUILD}.rd-compressed"`"

fi

[ -z "${KVER}" ] && die "ERROR: Configuration for model ${MODEL} and buildnumber ${BUILD} not found."

writeConfigKey "build" "${BUILD}" "${USER_CONFIG_FILE}"

declare -A SYNOINFO
declare -A ADDONS

# Read synoinfo and addons from config
while IFS="=" read KEY VALUE; do
  [ -n "${KEY}" ] && SYNOINFO["${KEY}"]="${VALUE}"
done < <(readConfigMap "synoinfo" "${USER_CONFIG_FILE}")
while IFS="=" read KEY VALUE; do
  [ -n "${KEY}" ] && ADDONS["${KEY}"]="${VALUE}"
done < <(readConfigMap "addons" "${USER_CONFIG_FILE}")

# Patches
while read f; do
  echo -n "."
  echo "Patching with ${f}" >"${LOG_FILE}" 2>&1
  (cd "${RAMDISK_PATH}" && patch -p1 < "${PATCH_PATH}/${f}") >>"${LOG_FILE}" 2>&1 || dieLog
done < <(readModelArray "${MODEL}" "builds.${BUILD}.patch")

# Patch /etc/synoinfo.conf
echo -n "."
for KEY in ${!SYNOINFO[@]}; do
  _set_conf_kv "${KEY}" "${SYNOINFO[${KEY}]}" "${RAMDISK_PATH}/etc/synoinfo.conf" >"${LOG_FILE}" 2>&1 || dieLog
done

# Patch /sbin/init.post
echo -n "."
grep -v -e '^[\t ]*#' -e '^$' "${PATCH_PATH}/config-manipulators.sh" > "${TMP_PATH}/rp.txt"
sed -e "/@@@CONFIG-MANIPULATORS-TOOLS@@@/ {" -e "r ${TMP_PATH}/rp.txt" -e 'd' -e '}' -i "${RAMDISK_PATH}/sbin/init.post"
rm "${TMP_PATH}/rp.txt"
touch "${TMP_PATH}/rp.txt"
for KEY in ${!SYNOINFO[@]}; do
  echo "_set_conf_kv '${KEY}' '${SYNOINFO[${KEY}]}' '/tmpRoot/etc/synoinfo.conf'" >> "${TMP_PATH}/rp.txt"
  echo "_set_conf_kv '${KEY}' '${SYNOINFO[${KEY}]}' '/tmpRoot/etc.defaults/synoinfo.conf'" >> "${TMP_PATH}/rp.txt"
done
sed -e "/@@@CONFIG-GENERATED@@@/ {" -e "r ${TMP_PATH}/rp.txt" -e 'd' -e '}' -i "${RAMDISK_PATH}/sbin/init.post"
rm "${TMP_PATH}/rp.txt"

echo -n "."
# Extract modules to ramdisk
rm -rf "${TMP_PATH}/modules"
mkdir -p "${TMP_PATH}/modules"
gzip -dc "${CACHE_PATH}/modules/${PLATFORM}-${KVER}.tgz" | tar xf - -C "${TMP_PATH}/modules"
for F in `ls "${TMP_PATH}/modules/"*.ko`; do
  M=`basename ${F}`
  # Skip existent modules
#  [ -f "${RAMDISK_PATH}/usr/lib/modules/${M}" ] || mv "${F}" "${RAMDISK_PATH}/usr/lib/modules/${M}"
  cp "${F}" "${RAMDISK_PATH}/usr/lib/modules/${M}"
done
mkdir -p "${RAMDISK_PATH}/usr/lib/firmware"
gzip -dc "${CACHE_PATH}/modules/firmware.tgz" | tar xf - -C "${RAMDISK_PATH}/usr/lib/firmware"
# Clean
rm -rf "${TMP_PATH}/modules"

echo -n "."
# Copying fake modprobe
cp "${PATCH_PATH}/iosched-trampoline.sh" "${RAMDISK_PATH}/usr/sbin/modprobe"
# Copying LKM to /usr/lib/modules
gzip -dc "${LKM_PATH}/rp-${PLATFORM}-${KVER}-${LKM}.ko.gz" > "${RAMDISK_PATH}/usr/lib/modules/rp.ko"

# Addons
MAXDISKS=`readConfigKey "maxdisks" "${USER_CONFIG_FILE}"`
# Check if model needs Device-tree dynamic patch
DT="`readModelKey "${MODEL}" "dt"`"

echo -n "."
mkdir -p "${RAMDISK_PATH}/addons"
echo "#!/bin/sh" > "${RAMDISK_PATH}/addons/addons.sh"
echo 'echo "addons.sh called with params ${@}"' >> "${RAMDISK_PATH}/addons/addons.sh"
# Required eudev and dtbpatch/maxdisks
installAddon eudev
echo "/addons/eudev.sh \${1} " >> "${RAMDISK_PATH}/addons/addons.sh" 2>"${LOG_FILE}" || dieLog
if [ "${DT}" = "true" ]; then
  installAddon dtbpatch
  echo "/addons/dtbpatch.sh \${1} " >> "${RAMDISK_PATH}/addons/addons.sh" 2>"${LOG_FILE}" || dieLog
else
  installAddon maxdisks
  echo "/addons/maxdisks.sh \${1} ${MAXDISKS}" >> "${RAMDISK_PATH}/addons/addons.sh" 2>"${LOG_FILE}" || dieLog
fi
# User addons
for ADDON in ${!ADDONS[@]}; do
  PARAMS=${ADDONS[${ADDON}]}
  if ! installAddon ${ADDON}; then
    echo "ADDON ${ADDON} not found!" | tee "${LOG_FILE}"
    exit 1
  fi
  echo "/addons/${ADDON}.sh \${1} ${PARAMS}" >> "${RAMDISK_PATH}/addons/addons.sh" 2>"${LOG_FILE}" || dieLog
done
chmod +x "${RAMDISK_PATH}/addons/addons.sh"

# Build modules dependencies
/opt/arpl/depmod -a -b ${RAMDISK_PATH} 2>/dev/null

# Reassembly ramdisk
echo -n "."
if [ "${RD_COMPRESSED}" == "true" ]; then
  (cd "${RAMDISK_PATH}" && find . | cpio -o -H newc -R root:root | xz -9 --format=lzma > "${MOD_RDGZ_FILE}") >"${LOG_FILE}" 2>&1 || dieLog
else
  (cd "${RAMDISK_PATH}" && find . | cpio -o -H newc -R root:root > "${MOD_RDGZ_FILE}") >"${LOG_FILE}" 2>&1 || dieLog
fi

# Clean
rm -rf "${RAMDISK_PATH}"

# Update SHA256 hash
RAMDISK_HASH="`sha256sum ${ORI_RDGZ_FILE} | awk '{print$1}'`"
writeConfigKey "ramdisk-hash" "${RAMDISK_HASH}" "${USER_CONFIG_FILE}"
echo

die "STOP test"
