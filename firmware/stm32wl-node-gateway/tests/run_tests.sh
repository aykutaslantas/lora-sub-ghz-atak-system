#!/usr/bin/env bash
# Firmware'in uygulama mantigini (subghz_phy_app.c) PC'de, sahte HAL/radyo/zamanlayici ile test eder.
# Gereken: gcc.  Calistirma:  cd tests && ./run_tests.sh
set -u
cd "$(dirname "$0")"
INC="-Istubs -I../SubGHz_Phy/App -I../Core/Inc"
CC="gcc -std=gnu11 -w $INC"
OUT=build; mkdir -p "$OUT"
fail=0
run() {   # run <isim> <kaynak> <bayraklar...>
  local name=$1 src=$2; shift 2
  if ! $CC "$@" -o "$OUT/$name" "$src" ${EXTRA:-} 2> "$OUT/$name.log"; then
    echo "[DERLEME HATASI] $name (bkz. $OUT/$name.log)"; fail=1; return
  fi
  local res; res=$("$OUT/$name" 2>&1 | tr -d '\r' | grep -E 'GECTI|HATA' | tail -1)
  printf '%-28s %s\n' "$name" "$res"
  [[ "$res" == *GECTI* ]] || fail=1
}
P="-DSECURE_ENABLED=0 -DSECURE_ONLY=0"
run crypto            test_crypto.c
EXTRA=flash_stub.c run mesh_relayer      test_mesh_node.c    $P -DROLE_RELAYER=1 -DGATEWAY_MODE=0 -DJOIN_ENABLED=0 -DTEST_FAR_NODE=0 -DUIDW0=0x1764
EXTRA=flash_stub.c run mesh_far_node     test_mesh_node.c    $P -DROLE_RELAYER=0 -DGATEWAY_MODE=0 -DJOIN_ENABLED=0 -DTEST_FAR_NODE=1 -DUIDW0=0x0F53
EXTRA=flash_stub.c run mesh_gateway      test_mesh_gateway.c $P -DGATEWAY_MODE=1 -DTEST_GW_IGNORE_DIRECT=0x0F53 -DGW_FORWARD_CMD=1 -DGW_FORWARD_RELAY_INFO=1 -DGATEWAY_LOCAL_TABLE=0 -DUIDW0=0x1764
run join_node         test_join.c         $P -DROLE_GW=0 -DGATEWAY_MODE=0 -DJOIN_ENABLED=1 -DTEST_FAR_NODE=0 -DUIDW0=0x9000ABCD
run join_gateway      test_join.c         $P -DROLE_GW=1 -DGATEWAY_MODE=1 -DTEST_GW_IGNORE_DIRECT=0 -DGW_FORWARD_CMD=1 -DGW_FORWARD_RELAY_INFO=1 -DGATEWAY_LOCAL_TABLE=0 -DUIDW0=0x1764
S="-DSECURE_ENABLED=1"
run secure_node       test_secure.c       $S -DSECURE_ONLY=0 -DROLE=0 -DGATEWAY_MODE=0 -DJOIN_ENABLED=0 -DTEST_FAR_NODE=0 -DUIDW0=0x1764
run secure_node_only  test_secure.c       $S -DSECURE_ONLY=1 -DROLE=0 -DGATEWAY_MODE=0 -DJOIN_ENABLED=0 -DTEST_FAR_NODE=0 -DUIDW0=0x1764
run secure_gateway    test_secure.c       $S -DSECURE_ONLY=0 -DROLE=1 -DGATEWAY_MODE=1 -DGW_FORWARD_CMD=1 -DGW_FORWARD_RELAY_INFO=1 -DGATEWAY_LOCAL_TABLE=0 -DTEST_GW_IGNORE_DIRECT=0 -DUIDW0=0x1764
run secure_join       test_secure.c       $S -DSECURE_ONLY=0 -DROLE=2 -DGATEWAY_MODE=0 -DJOIN_ENABLED=1 -DTEST_FAR_NODE=0 -DUIDW0=0x9000ABCD
run relay_sel_relayer test_relay_select.c $S -DSECURE_ONLY=1 -DROLE=0 -DGATEWAY_MODE=0 -DJOIN_ENABLED=0 -DTEST_FAR_NODE=0 -DUIDW0=0x1764
run relay_sel_far_node test_relay_select.c $S -DSECURE_ONLY=1 -DROLE=1 -DGATEWAY_MODE=0 -DJOIN_ENABLED=0 -DTEST_FAR_NODE=0 -DUIDW0=0x0F53
run secure_gw_only    test_secure_gw_only.c $S -DSECURE_ONLY=1 -DGATEWAY_MODE=1 -DGATEWAY_LOCAL_TABLE=0 -DTEST_GW_IGNORE_DIRECT=0 -DUIDW0=0x1764
echo
if [[ $fail == 0 ]]; then echo "TUM TESTLER GECTI"; else echo "BASARISIZ TEST VAR"; fi
exit $fail
