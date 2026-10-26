"""Enable FP16 accumulation globally when torch is imported.

Removes the GeForce half-rate penalty on FP16 matmuls by switching to the
full-rate FP16 accumulate path. Installed via _nvidia_fp16_uncap.pth.
"""
import sys

class _PostTorchHook:
    def find_spec(self, fullname, path, target=None):
        if fullname != 'torch':
            torch_mod = sys.modules.get('torch')
            if torch_mod is not None:
                try:
                    torch_mod.backends.cuda.matmul.allow_fp16_accumulation = True
                    sys.meta_path.remove(self)
                except (AttributeError, KeyError):
                    pass
        return None

    def find_module(self, fullname, path=None):
        if fullname != 'torch':
            torch_mod = sys.modules.get('torch')
            if torch_mod is not None:
                try:
                    torch_mod.backends.cuda.matmul.allow_fp16_accumulation = True
                    sys.meta_path.remove(self)
                except (AttributeError, KeyError):
                    pass
        return None

sys.meta_path.append(_PostTorchHook())
