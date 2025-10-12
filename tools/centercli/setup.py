from setuptools import setup

setup(
    name='centercli',
    version='1.0',
    py_modules=['centercli'],
    include_package_data=True,
    install_requires=[
        'click',
        'requests',
    ],
    entry_points='''
        [console_scripts]
          centercli=centercli:cli
    ''',
)

