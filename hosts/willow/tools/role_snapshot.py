"""Binary layout shared by Willow role snapshot import and reconciliation."""
from struct import Struct

CURRENT_VERSION = b"HEW6"
SUPPORTED_VERSIONS = (b"HEW2", b"HEW3", b"HEW4", b"HEW5", CURRENT_VERSION)
COUNTER_FIELDS = ("Clock", "Posted", "Pushed")
COUNTERS = Struct("<III")
MEMBER_TIMES = Struct("<II")
POST = Struct("<IB")
PREFERENCE_TEXT = (("Name", 31), ("guest", 63), ("admin", 63), ("owner_info", 119))
PREFERENCE_FIELDS = ("width", "flags", "acks", "loop", "flood_max_hops",
                     "unscoped_max_hops", "advert_max_hops", "local_advert_seconds",
                     "flood_advert_seconds")
PREFERENCE_SCALARS = Struct("<" + "I" * len(PREFERENCE_FIELDS))
PREFERENCE_DELAYS = Struct("<ff")
PREFERENCE_LOCATION = Struct("<ddqB")
PREFERENCE_SOURCE = Struct("<ffB")
