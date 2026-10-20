import numpy as np
import pytest
from analysis import paired,holm

def test_paired_constant_difference_and_exact_sign():
    s=paired([2,3,4,5],[1,2,3,4])
    assert s['mean_difference']==1 and s['se']==0
    assert s['sign_p']==.125
    assert s['t_ci_low']==s['t_ci_high']==1

def test_ties_and_pair_validation():
    assert paired([1,2],[1,2])['sign_p']==1
    with pytest.raises(ValueError):paired([1,2],[1])
    with pytest.raises(ValueError):paired([np.nan,2],[1,2])

def test_holm_known_family_and_original_order():
    np.testing.assert_allclose(holm([.04,.001,.03]),[.06,.003,.06])
