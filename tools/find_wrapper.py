"""一次性 helper: 找到 py-clob-client-v2 中 order_to_json_v2 函数的源码定义。"""
import inspect
import importlib
import pkgutil
import py_clob_client_v2

found_any = False
for mod_info in pkgutil.walk_packages(py_clob_client_v2.__path__, prefix='py_clob_client_v2.'):
    try:
        m = importlib.import_module(mod_info.name)
    except Exception:
        continue
    for fn_name in ('order_to_json_v2', 'order_to_json_v1', '_is_v2_order'):
        if hasattr(m, fn_name):
            obj = getattr(m, fn_name)
            try:
                src = inspect.getsource(obj)
            except Exception as e:
                src = f"(getsource failed: {e})"
            print(f"=== {fn_name}  in  {mod_info.name} ===")
            print(src)
            print()
            found_any = True

if not found_any:
    print("[!] none of order_to_json_v2 / order_to_json_v1 / _is_v2_order found at module level.")
    print("[!] Listing all modules and their public names containing 'order_to_json':")
    for mod_info in pkgutil.walk_packages(py_clob_client_v2.__path__, prefix='py_clob_client_v2.'):
        try:
            m = importlib.import_module(mod_info.name)
            for nm in dir(m):
                if 'order_to_json' in nm.lower():
                    print(f"  {mod_info.name}.{nm}")
        except Exception:
            pass
